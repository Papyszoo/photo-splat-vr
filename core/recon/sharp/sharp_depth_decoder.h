#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/progress.h"
#include "common/status.h"
#include "ggml.h"
#include "recon/ggml/dpt.h"
#include "recon/ggml/graph.h"
#include "recon/sharp/sharp_model.h"
#include "recon/sharp/sharp_stage.h"

namespace splat {

// C3 stage 3 (depth decoder) and stage 4 (depth adjustment).
//
// The decoder is Depth Pro's `MultiresConvDecoder` over the encoder's five
// levels, with `dims_decoder = (256, 256, 256, 256, 256)`:
//
//   convs[4] 3x3 1024->256 @48^2                                  (no bias)
//   fusions[4..1] FeatureFusionBlock2d: +resnet1(skip), resnet2,
//                2x2/2 transposed conv, 1x1                        (x2 each)
//   convs[0] is Identity (256 -> 256), fusions[0] has no upsampling
//   -> 256@768^2 decoder features
//
//   head: 3x3 256->128, TConv2x2 128->128, 3x3 128->32, ReLU,
//         1x1 32->2, ReLU                                          -> disparity
//   monodepth = disparity_factor / clamp(disparity, 1e-4, 1e4)      (metres)
//
// Stage 4 is the depth-adjustment U-Net, and it does **not** run at inference:
// `RGBGaussianPredictor.forward` always passes `depth=None`, so `DepthAlignment`
// takes its else branch and returns an all-ones map without touching
// `scale_map_estimator` (docs/research/sharp-reference.md §3 Q1). This port
// therefore applies no alignment at all — the monodepth is the decoder output —
// and the parity test checks the reference's own 040_depth_align_* dumps to
// confirm the stage really is the identity.
//
// The disparity and monodepth are 2 layers of (size, size) at the network
// resolution (1536^2), layer-major: index = (layer * size + y) * size + x.
struct SharpDepthConfig {
    // The decoder + head is ~60 nodes; one submit keeps the whole level-0
    // fusion (256@768^2) inside ggml's liveness planner instead of leaving a
    // 604 MiB copy in the cross-submit arena per fusion block.
    size_t nodes_per_submit = 512;
    // Production needs only the 2@1536^2 disparity (19 MiB); the parity path
    // additionally carries the 256@768^2 decoder output (604 MiB).
    size_t arena_bytes = 768u << 20;
};

struct SharpDepthResult {
    int image_size = 0;
    int layers = 2;                 // fixed by the checkpoint
    std::vector<float> disparity;   // layers * size * size
    std::vector<float> monodepth;   // layers * size * size, metric (normalised)
    double decoder_seconds = 0.0;
    double head_seconds = 0.0;
    double seconds = 0.0;
    size_t submits = 0;
    size_t arena_bytes = 0;
};

class SharpDepthDecoder {
public:
    ~SharpDepthDecoder();
    SharpDepthDecoder(const SharpDepthDecoder&) = delete;
    SharpDepthDecoder& operator=(const SharpDepthDecoder&) = delete;

    static Result<std::unique_ptr<SharpDepthDecoder>> create(std::shared_ptr<SharpModel> model,
                                                             SharpDepthConfig config = {});
    // Convenience form for a single stage; loads its own weights.
    static Result<std::unique_ptr<SharpDepthDecoder>> create(const Path& gguf, bool prefer_vulkan,
                                                             SharpDepthConfig config = {});

    // `levels` are the five encoder maps (index 0 finest), live in the model's
    // shared backend. `disparity_factor` is f_px / original_width.
    Result<SharpDepthResult> run(const std::array<ggml_tensor*, 5>& levels, int image_size,
                                 float disparity_factor,
                                 const SharpStageCallback& stage_cb = {},
                                 Progress* progress = nullptr);

    const GgmlBackend& backend() const { return model_->backend(); }
    const SharpDepthConfig& config() const { return config_; }

private:
    SharpDepthDecoder() = default;
    Status initialize(std::shared_ptr<SharpModel> model);

    std::shared_ptr<SharpModel> model_;
    SharpDepthConfig config_;
    // convs[0] is Identity in the reference and has no tensors; 1..4 are 3x3
    // convolutions without bias.
    std::array<ConvWeights, 5> convs_;
    std::array<FusionWeights, 5> fusions_;
    ConvWeights head_conv0_;
    ConvWeights head_deconv_;
    ConvWeights head_conv2_;
    ConvWeights head_last_;
};

}  // namespace splat
