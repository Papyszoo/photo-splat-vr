#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "ggml.h"
#include "recon/ggml/backend.h"
#include "recon/ggml/graph.h"
#include "recon/ggml/vit.h"
#include "recon/ggml/weights.h"
#include "recon/sharp/sharp_model.h"
#include "recon/sharp/sharp_stage.h"

namespace splat {

// C3 stage 2: Depth Pro's Sliding Pyramid Network on ggml, i.e. everything
// between the normalised 1536x1536 input and the five encoder feature maps the
// depth decoder consumes (docs/research/sharp-reference.md §6.1):
//
//   3-level pyramid -> 35 overlapping 384x384 patches
//   -> patch encoder (ViT-L/16, 24 blocks, taps at blocks 5 and 11 plus the
//      final normed output) in batches of `patch_batch`
//   -> trimmed sliding-window merge to 96x96 / 96x96 / 96x96 / 48x48 / 24x24
//   -> per-level 1x1 + transposed-conv upsamplers
//   -> image encoder (a second ViT-L) on the 384x384 level, fused into the
//      lowest-resolution output
//
// Shapes are fixed by the checkpoint; they are checked against the GGUF at
// load time rather than read from metadata (the SHARP GGUF carries none).
struct SharpEncoderConfig {
    int image_size = 1536;     // network resolution, 4 * patch_size
    int patch_size = 384;      // ViT input resolution
    // Patches per ViT submit. The reference runs all 35 at once; batching keeps
    // the MLP activation (B * 577 * 4096 floats) bounded, which is what spec
    // C3 asks for.
    int patch_batch = 4;
    size_t nodes_per_submit = 48;
    // Arena for one patch batch: the per-block carried tokens plus the three
    // tap snapshots.
    size_t batch_arena_bytes = 256u << 20;
    // Arena for the merged maps and the five level outputs (about 1.06 GiB of
    // live tensors at 1536x1536, level 0 alone is 604 MiB).
    size_t arena_bytes = 1280u << 20;
};

// One tensor read back for the parity test. `shape` is the ggml ne order, so a
// (W, H, C, 1) channels-last map has shape {W, H, C, 1}.
// (SharpStage is the shared type; the name stays for the stage 1-2 test.)
using SharpEncoderStage = SharpStage;

using SharpEncoderStageCallback = SharpStageCallback;

struct SharpEncoderResult {
    int image_size = 0;
    std::array<int, 5> level_channels{{256, 256, 512, 1024, 1024}};
    std::array<std::array<int, 2>, 5> level_sizes{{{{768, 768}}, {{384, 384}}, {{192, 192}},
                                                   {{96, 96}}, {{48, 48}}}};
    size_t submits = 0;       // every submit of the run, patch encoder included
    size_t arena_bytes = 0;   // peak of the persistent (cross-submit) arena
    double pyramid_seconds = 0.0;
    double patch_encoder_seconds = 0.0;
    double merge_seconds = 0.0;
    double image_encoder_seconds = 0.0;
    double upsample_seconds = 0.0;
    double seconds = 0.0;     // the whole stage
};

class SharpEncoder {
public:
    ~SharpEncoder();
    SharpEncoder(const SharpEncoder&) = delete;
    SharpEncoder& operator=(const SharpEncoder&) = delete;

    // prefer_vulkan=false forces the CPU backend, which is the fp32 parity
    // path. The GGUF must be the SHARP profile (architecture "sharp"). This form
    // loads the weights itself; the reconstructor shares one SharpModel with the
    // decoders through the overload below.
    static Result<std::unique_ptr<SharpEncoder>> create(const Path& gguf, bool prefer_vulkan,
                                                       SharpEncoderConfig config = {});
    static Result<std::unique_ptr<SharpEncoder>> create(std::shared_ptr<SharpModel> model,
                                                       SharpEncoderConfig config = {});

    // `input_chw` is 3 * image_size^2 floats, x fastest, already normalised to
    // [-1, 1] (sharp_preprocess + sharp_normalise). The five level maps stay
    // valid until the next run() or destroy.
    Result<SharpEncoderResult> run(const float* input_chw, int image_size,
                                   const SharpEncoderStageCallback& stage_cb = {},
                                   Progress* progress = nullptr);

    // Channels-last (W, H, C, 1); index 0 is the finest level (stride 2).
    ggml_tensor* level(int index) const;
    const GgmlBackend& backend() const { return model_->backend(); }
    const SharpEncoderConfig& config() const { return config_; }
    std::string metadata(const std::string& key) const { return model_->weights().metadata(key); }
    const std::shared_ptr<SharpModel>& model() const { return model_; }

private:
    SharpEncoder() = default;
    Status initialize(std::shared_ptr<SharpModel> model);

    std::shared_ptr<SharpModel> model_;
    SharpEncoderConfig config_;
    ViTConfig vit_config_;
    ViTNames patch_names_;
    ViTNames image_names_;
    ViTWeights patch_weights_;
    ViTWeights image_weights_;

    // One 1x1 projection followed by `count - 1` 2x2 transposed convolutions.
    struct Upsampler {
        std::vector<ggml_tensor*> weights;
        ggml_tensor* lowres_weight = nullptr;  // upsample_lowres (TConv, bias)
        ggml_tensor* lowres_bias = nullptr;
        ggml_tensor* fuse_weight = nullptr;    // fuse_lowres (1x1, bias)
        ggml_tensor* fuse_bias = nullptr;
    };
    Upsampler latent0_;
    Upsampler latent1_;
    Upsampler up0_;
    Upsampler up1_;
    Upsampler up2_;

    // Session of the last run(): owns the level tensors.
    std::unique_ptr<GraphSession> session_;
    std::array<ggml_tensor*, 5> levels_{{nullptr, nullptr, nullptr, nullptr, nullptr}};
};

}  // namespace splat
