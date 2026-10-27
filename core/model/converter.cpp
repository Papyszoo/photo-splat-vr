#include "model/converter.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

#include "common/half.h"
#include "model/gguf_writer.h"
#include "model/sha256.h"

namespace splat {
namespace {

constexpr char kTorchPrefix[] = "monodepth_model.monodepth_predictor.";

// The two timm ImageNet classifier heads the reference never calls: TimmViT
// overrides forward and stops at the pre-logits features
// (docs/research/sharp-reference.md §2).
constexpr const char* kDeadHeadSuffixes[] = {
    "encoder.patch_encoder.head.weight", "encoder.patch_encoder.head.bias",
    "encoder.image_encoder.head.weight", "encoder.image_encoder.head.bias"};

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lowercase(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

std::string basename_of(const Path& path) { return path.filename().string(); }

// Streams float32 elements from the zip into the GGUF writer as fp16, keeping
// only one chunk of output alive. Handles chunk boundaries that are not a
// multiple of four bytes by carrying up to three bytes over.
class Fp16Streamer {
public:
    explicit Fp16Streamer(GgufWriter& writer) : writer_(writer) { out_.reserve(1u << 16); }

    Status begin() { return writer_.begin_tensor(&expected_bytes_); }

    bool operator()(uint64_t, const void* data, size_t size) {
        if (!status_.ok()) return false;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t remaining = size;

        if (carry_len_ > 0) {
            while (carry_len_ < sizeof(carry_) && remaining > 0) {
                carry_[carry_len_++] = *bytes++;
                --remaining;
            }
            if (carry_len_ < sizeof(carry_)) return true;
            emit_element(carry_);
            carry_len_ = 0;
            if (!status_.ok()) return false;
        }

        const size_t elements = remaining / sizeof(carry_);
        if (elements > 0) {
            out_.resize(elements);
            for (size_t i = 0; i < elements; ++i) {
                float value = 0.0f;
                std::memcpy(&value, bytes + i * sizeof(carry_), sizeof(value));
                out_[i] = float_to_half(value);
            }
            status_ = writer_.append_tensor_bytes(out_.data(), elements * sizeof(uint16_t));
            if (!status_.ok()) return false;
            produced_ += elements;
            bytes += elements * sizeof(carry_);
            remaining -= elements * sizeof(carry_);
        }

        if (remaining > 0) {
            std::memcpy(carry_ + carry_len_, bytes, remaining);
            carry_len_ += remaining;
        }
        return true;
    }

    Status finish() {
        if (!status_.ok()) return status_;
        if (carry_len_ != 0) return Status::error("tensor payload is not a whole number of float32 elements");
        if (produced_ * sizeof(uint16_t) != expected_bytes_) {
            return Status::error("tensor produced " + std::to_string(produced_ * sizeof(uint16_t)) +
                                 " of " + std::to_string(expected_bytes_) + " bytes");
        }
        return writer_.end_tensor();
    }

    uint64_t produced() const { return produced_; }

private:
    void emit_element(const uint8_t* bytes) {
        float value = 0.0f;
        std::memcpy(&value, bytes, sizeof(value));
        const uint16_t half = float_to_half(value);
        status_ = writer_.append_tensor_bytes(&half, sizeof(half));
        if (!status_.ok()) return;
        ++produced_;
    }

    GgufWriter& writer_;
    std::vector<uint16_t> out_;
    uint8_t carry_[4] = {};
    size_t carry_len_ = 0;
    uint64_t produced_ = 0;
    uint64_t expected_bytes_ = 0;
    Status status_;
};

// 1-D tensors are copied through untouched; they stay fp32.
class PassthroughStreamer {
public:
    explicit PassthroughStreamer(GgufWriter& writer) : writer_(writer) {}

    Status begin() { return writer_.begin_tensor(&expected_bytes_); }

    bool operator()(uint64_t, const void* data, size_t size) {
        if (!status_.ok()) return false;
        status_ = writer_.append_tensor_bytes(data, size);
        written_ += size;
        return status_.ok();
    }

    Status finish() {
        if (!status_.ok()) return status_;
        if (written_ != expected_bytes_) {
            return Status::error("tensor wrote " + std::to_string(written_) + " of " +
                                 std::to_string(expected_bytes_) + " bytes");
        }
        return writer_.end_tensor();
    }

    uint64_t written() const { return written_; }

private:
    GgufWriter& writer_;
    uint64_t written_ = 0;
    uint64_t expected_bytes_ = 0;
    Status status_;
};

}  // namespace

bool is_dead_tensor(const std::string& torch_name) {
    if (torch_name.rfind("depth_alignment.", 0) == 0) return true;
    for (const char* suffix : kDeadHeadSuffixes) {
        if (ends_with(torch_name, suffix)) return true;
    }
    return false;
}

std::string gguf_tensor_name(const std::string& torch_name) {
    if (torch_name.rfind(kTorchPrefix, 0) == 0) return torch_name.substr(sizeof(kTorchPrefix) - 1);
    return torch_name;
}

Result<ConvertStats> convert_checkpoint(const Path& in, const Path& out, const ConvertOptions& options,
                                        Progress& progress) {
    const auto started = std::chrono::steady_clock::now();
    ConvertStats stats;

    std::error_code file_error;
    const uintmax_t source_bytes = std::filesystem::file_size(in, file_error);
    if (file_error) return Result<ConvertStats>::failure("cannot stat " + in.string() + ": " + file_error.message());
    stats.source_bytes = source_bytes;

    // The digest is always computed: it goes into the GGUF metadata. Rule 2
    // additionally requires it to match the pinned checkpoint.
    const Result<std::string> digest = sha256_file(in);
    if (!digest.ok()) return Result<ConvertStats>::failure(digest.message());
    stats.source_sha256 = lowercase(digest.value());
    if (options.verify_sha256 && stats.source_sha256 != kSharpCheckpointSha256) {
        return Result<ConvertStats>::failure(
            in.string() + ": SHA-256 " + stats.source_sha256 + " does not match the pinned " +
            kSharpCheckpointSha256 + " (spec rule 2); refusing to convert");
    }

    Result<Checkpoint> opened = open_checkpoint(in);
    if (!opened.ok()) return Result<ConvertStats>::failure(opened.message());
    Checkpoint& checkpoint = opened.value();
    stats.zip64 = checkpoint.zip64;

    progress.report(0.1f, "checkpoint verified");

    std::vector<GgufTensorDesc> descriptions;
    std::vector<const CheckpointTensor*> kept;
    std::vector<std::string> dropped_names;
    std::vector<std::string> full_names;
    descriptions.reserve(checkpoint.tensors.size());
    kept.reserve(checkpoint.tensors.size());
    full_names.reserve(checkpoint.tensors.size());

    for (const CheckpointTensor& tensor : checkpoint.tensors) {
        if (is_dead_tensor(tensor.record.name)) {
            dropped_names.push_back(tensor.record.name);
            continue;
        }
        if (tensor.record.shape.size() > 4) {
            return Result<ConvertStats>::failure("tensor '" + tensor.record.name + "' has rank " +
                                                 std::to_string(tensor.record.shape.size()) +
                                                 "; GGUF converts ranks 1..4");
        }
        GgufTensorDesc description;
        description.name = gguf_tensor_name(tensor.record.name);
        description.shape = tensor.record.shape;
        description.fp16 = !options.keep_fp32 && tensor.record.shape.size() >= 2;
        descriptions.push_back(std::move(description));
        full_names.push_back(tensor.record.name);
        kept.push_back(&tensor);
        stats.source_tensor_bytes += tensor.record.numel() * 4;
    }

    if (kept.empty()) return Result<ConvertStats>::failure("checkpoint has no convertible tensors");

    GgufWriter writer;
    Status status = writer.begin(out, std::move(descriptions));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());

    writer.set_string("general.architecture", "sharp");
    writer.set_string("general.name", "SHARP (Apple ml-sharp 2572gikvuh)");
    writer.set_string("sharp.backend", options.backend);
    writer.set_string("sharp.converter", "splat_convert");
    writer.set_u32("sharp.converter_version", 1);
    writer.set_string("sharp.source_sha256", stats.source_sha256);
    writer.set_string("sharp.source_filename", basename_of(in));
    writer.set_u64("sharp.source_bytes", stats.source_bytes);
    writer.set_u32("sharp.source_tensor_count", static_cast<uint32_t>(checkpoint.tensors.size()));
    writer.set_u32("sharp.tensor_count", static_cast<uint32_t>(kept.size()));
    writer.set_u32("sharp.dropped_tensor_count", static_cast<uint32_t>(dropped_names.size()));
    writer.set_strings("sharp.dropped_tensors", dropped_names);
    writer.set_strings("sharp.source_tensor_names", full_names);
    writer.set_string(
        "sharp.tensor_name_rule",
        "gguf names drop the 'monodepth_model.monodepth_predictor.' prefix to fit ggml's 63-byte "
        "tensor name limit; full torch names are listed in sharp.source_tensor_names");
    writer.set_strings("sharp.changes",
                       {options.keep_fp32
                            ? "dtype: every tensor kept as float32 (--fp32)"
                            : "dtype: tensors with 2 or more dimensions converted from float32 to float16",
                        "dtype: 1-D tensors (norm weights, norm biases, conv biases) kept as float32",
                        "layout: torch shape (d0..dn) stored as ggml ne (dn..d0); bytes unchanged, "
                        "row-major element order preserved, no transpose applied",
                        "dropped: 2 ViT classifier heads and the depth_alignment.* U-Net, which the "
                        "reference never runs at inference"});

    status = writer.write_header();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    stats.data_offset = writer.data_offset();

    const size_t total = kept.size();
    for (size_t i = 0; i < total; ++i) {
        const CheckpointTensor& tensor = *kept[i];
        const bool fp16 = !options.keep_fp32 && tensor.record.shape.size() >= 2;
        if (fp16) {
            Fp16Streamer streamer(writer);
            status = streamer.begin();
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
            status = read_checkpoint_tensor(checkpoint.zip, tensor, [&](uint64_t offset, const void* data, size_t size) {
                return streamer(offset, data, size);
            });
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
            status = streamer.finish();
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
            if (streamer.produced() != tensor.record.numel()) {
                return Result<ConvertStats>::failure("tensor '" + tensor.record.name + "' produced " +
                                                     std::to_string(streamer.produced()) + " of " +
                                                     std::to_string(tensor.record.numel()) + " values");
            }
        } else {
            PassthroughStreamer streamer(writer);
            status = streamer.begin();
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
            status = read_checkpoint_tensor(checkpoint.zip, tensor, [&](uint64_t offset, const void* data, size_t size) {
                return streamer(offset, data, size);
            });
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
            status = streamer.finish();
            if (!status.ok()) return Result<ConvertStats>::failure(status.message());
        }
        progress.report(0.1f + 0.9f * static_cast<float>(i + 1) / static_cast<float>(total),
                        "converting tensors");
    }

    status = writer.finish();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    stats.gguf_bytes = writer.bytes_written();
    stats.tensor_count = kept.size();
    stats.dropped_count = dropped_names.size();

    const auto finished = std::chrono::steady_clock::now();
    stats.seconds = std::chrono::duration<double>(finished - started).count();
    return Result<ConvertStats>::success(std::move(stats));
}

}  // namespace splat
