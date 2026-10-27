#pragma once

#include <array>
#include <string>
#include <vector>

#include "ggml.h"
#include "recon/ggml/graph.h"
#include "recon/ggml/weights.h"

namespace splat {

// DPT neck + depth head, shared by C4 (DA2: 4 levels, 64 features, 32 hidden)
// and C3 (SHARP: same dump structure, different channel counts and names).
// The block sequence is the reference one:
//
//   reassemble[i] = 1x1 project -> resize (TConv x4 / TConv x2 / identity /
//                   3x3 stride 2)
//   neck[i]       = 3x3 conv to `features`
//   fusion[0..3]  = FeatureFusionBlock: (optional skip) residual 3x3 unit x2 -> 1x1
//                   -> bilinear upsample (align_corners=True) to the next size
//   head          = 3x3 -> bilinear to (14*patch_w, 14*patch_h) -> 3x3 -> ReLU
//                   -> 1x1 -> ReLU
struct DptConfig {
    int levels = 4;
    // Every tap out of the encoder is `embed_dim` wide (384 for DA2-Small);
    // the reassemble 1x1 projections are what narrow it per level.
    std::array<int, 4> in_channels{{384, 384, 384, 384}};
    std::array<int, 4> reassemble_channels{{48, 96, 192, 384}};
    int features = 64;
    int head_hidden = 32;
    int patch_size = 14;
};

// `%d` is replaced by the level index (0 shallowest).
struct DptNames {
    std::string project = "reassemble.%d.proj";
    std::string resize = "reassemble.%d.resize";
    std::string neck = "neck.convs.%d";
    std::string fusion_out = "fusion.%d.out_conv";
    std::string fusion_res1 = "fusion.%d.res1.conv%d";
    std::string fusion_res2 = "fusion.%d.res2.conv%d";
    std::string head_conv1 = "head.conv1";
    std::string head_conv2 = "head.conv2";
    std::string head_conv3 = "head.conv3";
};

struct ConvWeights {
    ggml_tensor* weight = nullptr;
    ggml_tensor* bias = nullptr;
};

// `prefix.weight` is required; `prefix.bias` is optional unless
// `bias_expected`. Throws (via require()) when the GGUF does not match the
// graph, so every caller reports the same missing-tensor message.
ConvWeights resolve_conv(const GgmlWeights& weights, const std::string& prefix,
                         bool bias_expected = false);

struct FusionWeights {
    ConvWeights out_conv;
    ConvWeights res1_conv1;
    ConvWeights res1_conv2;
    ConvWeights res2_conv1;
    ConvWeights res2_conv2;
    // 2x2/2 transposed conv used by SHARP's decoders to upsample inside the
    // fusion block (the DA2 DPT neck resizes with a separate reassemble step and
    // bilinear interpolation, so it leaves this empty).
    ConvWeights deconv;
};

struct DptWeights {
    std::vector<ConvWeights> projects;
    std::vector<ConvWeights> resizes;
    std::vector<ConvWeights> necks;
    std::vector<FusionWeights> fusions;
    ConvWeights head_conv1;
    ConvWeights head_conv2;
    ConvWeights head_conv3;
};

DptWeights resolve_dpt_weights(const GgmlWeights& weights, const DptConfig& config,
                               const DptNames& names);

struct DptOutput {
    std::vector<ggml_tensor*> layers;  // index 0 shallowest (4x)
    std::vector<ggml_tensor*> paths;   // paths[0] = path_4 (deepest), paths[3] = path_1
    ggml_tensor* conv1 = nullptr;
    ggml_tensor* depth = nullptr;  // (W, H, 1, 1), network resolution
};

// patch_maps: 4 channels-last maps (patch_w, patch_h, dim, 1) from the encoder
// taps, shallowest first. keep_stages forces a submit boundary at each layer /
// path / conv1 so a parity test can read them back.
DptOutput dpt_forward(GraphSession& session, const DptWeights& weights, const DptConfig& config,
                      const std::vector<ggml_tensor*>& patch_maps, int patch_w, int patch_h,
                      bool keep_stages);

// One FeatureFusionBlock2d as SHARP's MultiresConvDecoder spells it:
//
//   x = resnet2(x + resnet1(skip))       (residual 3x3 units, no norm, bias=True)
//   x = deconv(x)                        (2x2/2 transposed conv, no bias, optional)
//   x = out_conv(x)                      (1x1)
//
// `skip` may be null (the deepest block has no skip input). The DA2 neck shares
// the residual units and the 1x1 but resizes with bilinear interpolation before
// the out_conv, so it keeps its own sequence.
FusionWeights resolve_fusion_block(const GgmlWeights& weights, const std::string& prefix);
ggml_tensor* fusion_block(ggml_context* ctx, const FusionWeights& weights, ggml_tensor* x,
                          ggml_tensor* skip, bool upsample);

}  // namespace splat
