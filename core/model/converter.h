#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/checkpoint.h"

namespace splat {

struct ConvertOptions {
    // Spec rule 2: verify the pinned SHA-256 before converting. Only tests of
    // the fixtures turn this off.
    bool verify_sha256 = true;
    std::string backend = "sharp";
    // Keep every tensor in fp32 instead of the fp16 dtype rule (2-D and higher
    // -> fp16). The parity tests consume the fp32 GGUF; the runtime uses the
    // default fp16 one. Applies to both profiles.
    bool keep_fp32 = false;
    // The SHA-256 of `in` the caller already computed (the manager's
    // hash-while-downloading, or a verify pass it ran for another reason). Empty:
    // the converter hashes the file itself. Either way stats.source_sha256 is
    // the digest that goes into the GGUF metadata and, with verify_sha256, the
    // one checked against the pin: the file is never trusted unverified.
    std::string source_sha256;
    // One line every ~5 s of tensor I/O and one at the end: bytes read, bytes
    // written and MB/s, so two console runs can be compared. Empty: no lines.
    std::function<void(const std::string&)> log;
};

struct ConvertStats {
    std::string source_sha256;
    uint64_t source_bytes = 0;
    uint64_t source_tensor_bytes = 0;  // fp32 bytes read from the checkpoint
    uint64_t gguf_bytes = 0;
    uint64_t tensor_count = 0;   // tensors written
    uint64_t dropped_count = 0;  // tensors skipped
    uint64_t data_offset = 0;
    bool zip64 = false;
    double seconds = 0.0;
};

// Converts a torch.save SHARP checkpoint into a GGUF at `out`.
//
// Tensors with two or more dimensions become fp16; 1-D tensors (LayerNorm
// weights/biases, conv biases) stay fp32. The two dead ViT classifier heads and
// the whole depth_adjustment subtree are dropped: the U-Net is never evaluated
// at inference (docs/research/sharp-reference.md §3 Q1).
Result<ConvertStats> convert_checkpoint(const Path& in, const Path& out, const ConvertOptions& options,
                                        Progress& progress);

// True for tensors the converter drops.
bool is_dead_tensor(const std::string& torch_name);

// GGUF tensor name for a torch state-dict key. ggml caps tensor names at 63
// bytes and torch names reach 84, so the constant module prefix is stripped.
std::string gguf_tensor_name(const std::string& torch_name);

}  // namespace splat
