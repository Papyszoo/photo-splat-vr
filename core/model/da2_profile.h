#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/converter.h"

namespace splat {

// Spec C4 fallback model: Depth Anything V2 Small, the only V2 size that is
// Apache-2.0. Pinned identity, verified before every conversion (spec rule 2
// and C4 "fetch them the same way as SHARP's").
//
//   https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf
//
// The Hugging Face safetensors file is used instead of the original
// depth_anything_v2_vits.pth: 99 MB of raw little-endian float32 instead of a
// 99 MB torch zip/pickle, so no unpickler is needed and the checkpoint parses
// with the standard safetensors header.
inline constexpr char kDepthAnythingV2SmallName[] = "Depth-Anything-V2-Small-hf";
inline constexpr char kDepthAnythingV2SmallUrl[] =
    "https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf/resolve/main/model.safetensors";
inline constexpr char kDepthAnythingV2SmallSha256[] =
    "3152477ce0d8d6978d76b995120de97cb5b928701fd0f817769f59e249a16b70";
inline constexpr uint64_t kDepthAnythingV2SmallBytes = 99173660ull;
inline constexpr uint32_t kDepthAnythingV2SmallTensorCount = 287;

// Smallest valid input side and patch size of the DINOv2 ViT-S/14 backbone.
inline constexpr int kDepthAnythingV2PatchSize = 14;
inline constexpr int kDepthAnythingV2InputSize = 518;

// True for tensors the converter drops: the masked-image token is only used
// when masks are passed (never at inference) but is part of the module.
bool is_da2_dead_tensor(const std::string& hf_name);

// GGUF tensor name for a Hugging Face safetensors key. Keeps names short and
// stable for the ggml graph: backbone.encoder.layer.N.* -> blocks.N.*,
// neck.reassemble_stage.* -> reassemble.*, neck.fusion_stage.* -> fusion.*.
std::string da2_gguf_tensor_name(const std::string& hf_name);

// Converts the official safetensors checkpoint to GGUF. Same code path and
// policy as the SHARP converter (C1): 2-D and higher tensors become fp16,
// 1-D tensors stay fp32, tensors stream one at a time so peak memory stays at
// one chunk. `keep_fp32` forces fp32 everywhere, which is what the parity test
// consumes.
Result<ConvertStats> convert_depth_anything_v2(const Path& in, const Path& out,
                                              const ConvertOptions& options, Progress& progress);

}  // namespace splat
