#include "model/converter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "model/file_util.h"
#include "model/gguf_writer.h"
#include "model/io_hint.h"
#include "model/sha256.h"
#include "model/tensor_stream.h"
#include "model/text_format.h"

namespace splat {
namespace {

constexpr char kTorchPrefix[] = "monodepth_model.monodepth_predictor.";

// The converter's progress line cadence (issue-1-crash sharp-slow-report.md
// §5): one line per ~5 s of tensor I/O, plus one at the end.
constexpr std::chrono::milliseconds kLogInterval{5000};

// The two timm ImageNet classifier heads the reference never calls: TimmViT
// overrides forward and stops at the pre-logits features
// (docs/research/sharp-reference.md §2).
constexpr const char* kDeadHeadSuffixes[] = {
    "encoder.patch_encoder.head.weight", "encoder.patch_encoder.head.bias",
    "encoder.image_encoder.head.weight", "encoder.image_encoder.head.bias"};

std::string lowercase(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

}  // namespace

void write_sharp_gguf_metadata(GgufWriter& writer, const std::string& backend,
                               const std::string& source_sha256, const std::string& source_filename,
                               uint64_t source_bytes, uint32_t source_tensor_count,
                               uint32_t tensor_count, uint32_t dropped_tensor_count,
                               const std::vector<std::string>& dropped_tensors,
                               const std::vector<std::string>& source_tensor_names,
                               bool keep_fp32) {
    // The call order is part of the file's byte layout: the file converter and
    // the streaming converter share this function so their output is identical.
    writer.set_string("general.architecture", "sharp");
    writer.set_string("general.name", "SHARP (Apple ml-sharp 2572gikvuh)");
    writer.set_string("sharp.backend", backend);
    writer.set_string("sharp.converter", "splat_convert");
    writer.set_u32("sharp.converter_version", 1);
    writer.set_string("sharp.source_sha256", source_sha256);
    writer.set_string("sharp.source_filename", source_filename);
    writer.set_u64("sharp.source_bytes", source_bytes);
    writer.set_u32("sharp.source_tensor_count", source_tensor_count);
    writer.set_u32("sharp.tensor_count", tensor_count);
    writer.set_u32("sharp.dropped_tensor_count", dropped_tensor_count);
    writer.set_strings("sharp.dropped_tensors", dropped_tensors);
    writer.set_strings("sharp.source_tensor_names", source_tensor_names);
    writer.set_string(
        "sharp.tensor_name_rule",
        "gguf names drop the 'monodepth_model.monodepth_predictor.' prefix to fit ggml's 63-byte "
        "tensor name limit; full torch names are listed in sharp.source_tensor_names");
    writer.set_strings("sharp.changes",
                       {keep_fp32
                            ? "dtype: every tensor kept as float32 (--fp32)"
                            : "dtype: tensors with 2 or more dimensions converted from float32 to float16",
                        "dtype: 1-D tensors (norm weights, norm biases, conv biases) kept as float32",
                        "layout: torch shape (d0..dn) stored as ggml ne (dn..d0); bytes unchanged, "
                        "row-major element order preserved, no transpose applied",
                        "dropped: 2 ViT classifier heads and the depth_alignment.* U-Net, which the "
                        "reference never runs at inference"});
}

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

    write_sharp_gguf_metadata(writer, options.backend, stats.source_sha256, basename_of(in),
                              stats.source_bytes, static_cast<uint32_t>(checkpoint.tensors.size()),
                              static_cast<uint32_t>(kept.size()),
                              static_cast<uint32_t>(dropped_names.size()), dropped_names, full_names,
                              options.keep_fp32);

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
