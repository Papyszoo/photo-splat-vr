#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "recon/ggml/backend.h"
#include "recon/ggml/dpt.h"
#include "recon/ggml/stage.h"
#include "recon/ggml/vit.h"
#include "recon/ggml/weights.h"

namespace splat {

// C4 network: Depth Anything V2 Small on the shared ggml blocks.
//
//   preprocess (host, cv2-compatible cubic resize + ImageNet normalise)
//   -> DINOv2 ViT-S/14 encoder (12 blocks, 6 heads, dim 384, taps at 2/5/8/11)
//   -> DPT reassemble + 4 fusion blocks + depth head
//   -> relative inverse depth (ReLU'd), network resolution
//
// The same ViT block and DPT block serve the SHARP port; nothing in this file
// hard-codes the DA2 checkpoint beyond the config values below, which are read
// back from the GGUF metadata and checked against the file.

// One tensor read back for the parity test. `shape` is in ggml order (ne0
// fastest), which is the transpose of the torch/NCHW golden files; the type is
// shared with C3 (recon/ggml/stage.h).
using DepthAnythingStage = StageDump;

using DepthAnythingStageCallback = StageCallback;

struct DepthAnythingResult {
    int width = 0;   // network resolution
    int height = 0;
    std::vector<float> depth;  // width * height, x fastest (row-major image)
    size_t submits = 0;
    size_t arena_bytes = 0;  // peak of the persistent (cross-submit) tensor arena
    double seconds = 0.0;
};

class DepthAnythingV2 {
public:
    ~DepthAnythingV2();
    DepthAnythingV2(const DepthAnythingV2&) = delete;
    DepthAnythingV2& operator=(const DepthAnythingV2&) = delete;

    // prefer_vulkan=false forces the CPU backend (parity runs). `nodes_per_submit`
    // is the graph split budget from spec risk "long GPU submissions".
    static Result<std::unique_ptr<DepthAnythingV2>> create(const Path& gguf, bool prefer_vulkan,
                                                          size_t nodes_per_submit = 64,
                                                          size_t arena_bytes = 384u << 20);

    // `input_chw` is 3 * width * height floats, image-normalised, CHW. width and
    // height must be multiples of 14. stage_cb, when set, receives the patch
    // embedding, the four tapped blocks, the normed taps and every DPT stage.
    Result<DepthAnythingResult> run(const float* input_chw, int width, int height,
                                    const DepthAnythingStageCallback& stage_cb = {},
                                    Progress* progress = nullptr);

    const GgmlBackend& backend() const { return backend_; }
    const ViTConfig& vit_config() const { return vit_config_; }
    const DptConfig& dpt_config() const { return dpt_config_; }
    // Position-embedding grid of the checkpoint (37x37 for DINOv2 at 518).
    int position_grid() const { return position_grid_; }
    std::string metadata(const std::string& key) const { return weights_.metadata(key); }

private:
    DepthAnythingV2() = default;
    Status initialize(const Path& gguf, bool prefer_vulkan, size_t nodes_per_submit,
                      size_t arena_bytes);

    GgmlBackend backend_;
    GgmlWeights weights_;
    ViTConfig vit_config_;
    ViTNames vit_names_;
    DptConfig dpt_config_;
    DptNames dpt_names_;
    ViTWeights vit_weights_;
    DptWeights dpt_weights_;
    int position_grid_ = 37;
    size_t nodes_per_submit_ = 64;
    size_t arena_bytes_ = 384u << 20;
};

// Host preprocessing that mirrors DepthAnythingV2.image2tensor: cubic resize to
// a short side of 518 keeping the aspect ratio (both sides rounded to a multiple
// of 14) and ImageNet normalisation. Returns CHW floats.
Result<std::vector<float>> da2_preprocess(const uint8_t* rgb, int width, int height, int* net_width,
                                          int* net_height, int input_size = 518);
// cv2-compatible cubic resize (INTER_CUBIC, half-pixel centres, no antialias).
Result<std::vector<float>> resize_cubic_rgb(const uint8_t* rgb, int width, int height,
                                            int out_width, int out_height);

}  // namespace splat
