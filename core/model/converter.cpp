#include "model/converter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "common/half.h"
#include "model/gguf_writer.h"
#include "model/io_hint.h"
#include "model/sha256.h"

namespace splat {
namespace {

constexpr char kTorchPrefix[] = "monodepth_model.monodepth_predictor.";

// The converter's progress line cadence (issue-1-crash sharp-slow-report.md
// §5): one line per ~5 s of tensor I/O, plus one at the end.
constexpr std::chrono::milliseconds kLogInterval{5000};

std::string gigabyte_text(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

std::string rate_text(uint64_t bytes, double seconds) {
    char text[32];
    if (seconds <= 0.0) return "? MB/s";
    std::snprintf(text, sizeof(text), "%.1f MB/s", static_cast<double>(bytes) / seconds / 1e6);
    return text;
}

std::string seconds_text(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

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

    // The digest is always part of the GGUF metadata, and with verify_sha256 it
    // has to match the pinned checkpoint (spec rule 2). A caller that just
    // hashed the file (the manager's hash-while-downloading) hands it in, so the
    // 2.8 GB checkpoint is not read a second time (converter.cpp's old :176
    // pass).
    std::string digest =
        options.source_sha256.empty() ? std::string() : lowercase(options.source_sha256);
    if (digest.empty()) {
        const Result<std::string> hashed = sha256_file(in);
        if (!hashed.ok()) return Result<ConvertStats>::failure(hashed.message());
        digest = lowercase(hashed.value());
    }
    stats.source_sha256 = digest;
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
        kept.push_back(&tensor);
        stats.source_tensor_bytes += tensor.record.numel() * 4;
    }

    if (kept.empty()) return Result<ConvertStats>::failure("checkpoint has no convertible tensors");

    // File order, not state-dict order: the GGUF tensor order is free (every
    // reader looks a tensor up by name), while the checkpoint is then one
    // forward sweep instead of 91 jumps across 2.8 GB. This is the read side of
    // runs/issue-1-crash/sharp-slow-report.md §1.4; the output's tensor order is
    // the documented, intended difference from 0.6.0.
    std::sort(kept.begin(), kept.end(), [](const CheckpointTensor* a, const CheckpointTensor* b) {
        return a->zip_offset < b->zip_offset;
    });

    for (const CheckpointTensor* tensor : kept) {
        GgufTensorDesc description;
        description.name = gguf_tensor_name(tensor->record.name);
        description.shape = tensor->record.shape;
        description.fp16 = !options.keep_fp32 && tensor->record.shape.size() >= 2;
        descriptions.push_back(std::move(description));
        full_names.push_back(tensor->record.name);
    }

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
    const auto log_line = options.log;
    auto last_log = started;
    uint64_t tensors_read_bytes = 0;  // checkpoint bytes the loop has consumed
    // Page-cache batches of the read side (io_hint.h): the tensor data now
    // arrives in file order, so a 64 MiB window of it is dropped behind the
    // read head instead of competing with the renderer for memory.
    uint64_t drop_from = 0;
    uint64_t pending_drop = 0;
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

        const uint64_t tensor_bytes = tensor.record.numel() * 4;
        tensors_read_bytes += tensor_bytes;
        if (pending_drop == 0) drop_from = tensor.zip_offset;
        pending_drop += tensor_bytes;
        if (pending_drop >= kCacheBatchBytes) {
            drop_file_cache(in, drop_from, pending_drop);
            pending_drop = 0;
        }

        if (log_line) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_log >= kLogInterval || i + 1 == total) {
                const double elapsed = std::chrono::duration<double>(now - started).count();
                log_line("converter: " + std::to_string(i + 1) + "/" + std::to_string(total) +
                         " tensors, " + gigabyte_text(tensors_read_bytes) + " read, " +
                         gigabyte_text(writer.bytes_written() - stats.data_offset) +
                         " written, " + seconds_text(elapsed) + " s (" +
                         rate_text(tensors_read_bytes, elapsed) + " read, " +
                         rate_text(writer.bytes_written() - stats.data_offset, elapsed) +
                         " write)");
                last_log = now;
            }
        }
    }
    if (pending_drop > 0) drop_file_cache(in, drop_from, pending_drop);

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
