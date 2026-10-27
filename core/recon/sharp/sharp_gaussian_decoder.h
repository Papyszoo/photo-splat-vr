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

// C3 stage 5: the Gaussian decoder (`GaussianDensePredictionTransformer`).
//
//   skip  = image_encoder.conv 2x2/2  5 -> 128 @768^2            (feature_input)
//   feats = MultiresConvDecoder over the *encoder* levels, dims 128
//           (convs[0] = 1x1 256->128, convs[1..4] 3x3 -> 128,
//            fusions[0..4] FeatureFusionBlock2d with 2x2/2 upsampling)
//   feats = fusion(feats, skip)                                  (no deconv)
//   texture/geometry head: 2x [GroupNorm(8) -> ReLU -> 3x3 128->64 ->
//                              GroupNorm(8) -> ReLU -> 3x3 64->128] residual,
//                          ReLU, 1x1 128->32, ReLU
//   prediction_head: 1x1 32->6 (mean, scale) and 1x1 32->22
//                    (quaternion, colour, opacity), unflattened to
//                    (3, layers) / (11, layers) and concatenated -> (14, 2)
//
// The output is the *delta* tensor in torch (14, 2, grid, grid) order: the
// channel of each 1x1 head is `attribute * layers + layer`, and the composer
// (sharp_composer) turns it into Gaussians.
struct SharpGaussianConfig {
    size_t nodes_per_submit = 512;
    // Production carries the 32-channel head outputs (151 MiB) plus the
    // 28-channel delta (66 MiB); the parity path also carries the intermediate
    // 128-channel fused feature (302 MiB each). 1 GiB covers both.
    size_t arena_bytes = 1024u << 20;
};

// A `residual_block_2d(dim, dim, hidden = dim / 2, group_norm, 8 groups)`.
struct SharpGroupNormResidual {
    ConvWeights norm1;   // GroupNorm weight/bias (length = channels)
    ConvWeights conv1;   // 3x3, bias
    ConvWeights norm2;
    ConvWeights conv2;
};

struct SharpHeadWeights {
    SharpGroupNormResidual block[2];
    ConvWeights out;     // 1x1 -> 32, bias
};

struct SharpGaussianResult {
    int grid = 0;     // 768
    int layers = 2;   // fixed by the checkpoint
    // 14 * layers * grid * grid in torch (14, layers, grid, grid) order, i.e.
    // index = ((attribute * layers + layer) * grid + y) * grid + x.
    std::vector<float> delta;
    // Filled by the stage callback directly; kept in the result only so the
    // caller can inspect them (parity dumps 060_*). Not part of a normal run.
    std::vector<float> texture;
    std::vector<float> geometry;
    double seconds = 0.0;
    size_t submits = 0;
    size_t arena_bytes = 0;
};

class SharpGaussianDecoder {
public:
    ~SharpGaussianDecoder();
    SharpGaussianDecoder(const SharpGaussianDecoder&) = delete;
    SharpGaussianDecoder& operator=(const SharpGaussianDecoder&) = delete;

    static Result<std::unique_ptr<SharpGaussianDecoder>> create(std::shared_ptr<SharpModel> model,
                                                                SharpGaussianConfig config = {});
    static Result<std::unique_ptr<SharpGaussianDecoder>> create(const Path& gguf, bool prefer_vulkan,
                                                                SharpGaussianConfig config = {});

    // `feature_input` is the initialiser's 5-channel input, CHW at the network
    // resolution (sharp_initialize).
    Result<SharpGaussianResult> run(const std::array<ggml_tensor*, 5>& levels,
                                    const float* feature_input, int image_size,
                                    const SharpStageCallback& stage_cb = {},
                                    Progress* progress = nullptr);

    const GgmlBackend& backend() const { return model_->backend(); }
    const SharpGaussianConfig& config() const { return config_; }

private:
    SharpGaussianDecoder() = default;
    Status initialize(std::shared_ptr<SharpModel> model);

    std::shared_ptr<SharpModel> model_;
    SharpGaussianConfig config_;
    ConvWeights image_encoder_;
    std::array<ConvWeights, 5> convs_;
    std::array<FusionWeights, 5> fusions_;
    FusionWeights fusion_;
    SharpHeadWeights texture_head_;
    SharpHeadWeights geometry_head_;
    ConvWeights prediction_geometry_;
    ConvWeights prediction_texture_;
};

}  // namespace splat
