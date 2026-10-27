#include "recon/ggml/dpt.h"

#include <stdexcept>
#include <string>

#include "recon/ggml/check.h"
#include "recon/ggml/nn.h"

namespace splat {
namespace {

std::string format_pair(const std::string& pattern, int first, int second) {
    std::string text = pattern;
    size_t position = text.find("%d");
    if (position != std::string::npos) {
        text = text.substr(0, position) + std::to_string(first) + text.substr(position + 2);
    }
    position = text.find("%d");
    if (position != std::string::npos) {
        text = text.substr(0, position) + std::to_string(second) + text.substr(position + 2);
    }
    return text;
}

// ResidualConvUnit: out = x + conv2(relu(conv1(relu(x)))), no norm.
ggml_tensor* residual_unit(ggml_context* ctx, const ConvWeights& conv1, const ConvWeights& conv2,
                           ggml_tensor* x) {
    ggml_tensor* out = ggml_relu(ctx, x);
    out = nn::conv2d(ctx, conv1.weight, conv1.bias, out, 1, 1);
    out = ggml_relu(ctx, out);
    out = nn::conv2d(ctx, conv2.weight, conv2.bias, out, 1, 1);
    return ggml_add(ctx, out, x);
}

ggml_tensor* upsample_to(ggml_context* ctx, ggml_tensor* x, int64_t width, int64_t height) {
    return ggml_interpolate(ctx, x, width, height, x->ne[2], x->ne[3],
                            GGML_SCALE_MODE_BILINEAR | GGML_SCALE_FLAG_ALIGN_CORNERS);
}

}  // namespace

ConvWeights resolve_conv(const GgmlWeights& weights, const std::string& prefix, bool bias_expected) {
    ConvWeights conv;
    conv.weight = weights.need(prefix + ".weight");
    conv.bias = weights.find(prefix + ".bias");
    require(!bias_expected || conv.bias != nullptr, "missing GGUF tensor '" + prefix + ".bias'");
    return conv;
}

DptWeights resolve_dpt_weights(const GgmlWeights& weights, const DptConfig& config,
                               const DptNames& names) {
    DptWeights resolved;
    resolved.projects.reserve(config.levels);
    resolved.resizes.reserve(config.levels);
    resolved.necks.reserve(config.levels);
    resolved.fusions.reserve(config.levels);
    for (int level = 0; level < config.levels; ++level) {
        resolved.projects.push_back(resolve_conv(weights, format_pair(names.project, level, 0)));
        const std::string resize = format_pair(names.resize, level, 0);
        ConvWeights resize_weights;
        if (weights.find(resize + ".weight") != nullptr) {
            resize_weights = resolve_conv(weights, resize);
        }
        resolved.resizes.push_back(resize_weights);
        resolved.necks.push_back(resolve_conv(weights, format_pair(names.neck, level, 0)));
    }
    for (int index = 0; index < config.levels; ++index) {
        FusionWeights fusion;
        fusion.out_conv = resolve_conv(weights, format_pair(names.fusion_out, index, 0));
        fusion.res1_conv1 = resolve_conv(weights, format_pair(names.fusion_res1, index, 1));
        fusion.res1_conv2 = resolve_conv(weights, format_pair(names.fusion_res1, index, 2));
        fusion.res2_conv1 = resolve_conv(weights, format_pair(names.fusion_res2, index, 1));
        fusion.res2_conv2 = resolve_conv(weights, format_pair(names.fusion_res2, index, 2));
        resolved.fusions.push_back(fusion);
    }
    resolved.head_conv1 = resolve_conv(weights, names.head_conv1);
    resolved.head_conv2 = resolve_conv(weights, names.head_conv2);
    resolved.head_conv3 = resolve_conv(weights, names.head_conv3);
    return resolved;
}

DptOutput dpt_forward(GraphSession& session, const DptWeights& weights, const DptConfig& config,
                      const std::vector<ggml_tensor*>& patch_maps, int patch_w, int patch_h,
                      bool keep_stages) {
    require(static_cast<int>(patch_maps.size()) == config.levels,
            "dpt_forward needs one map per level");
    ggml_context* ctx = session.ctx();
    DptOutput output;
    output.layers.resize(static_cast<size_t>(config.levels));

    for (int level = 0; level < config.levels; ++level) {
        ggml_tensor* map = patch_maps[static_cast<size_t>(level)];
        require(map != nullptr && map->ne[2] == config.in_channels[level],
                "dpt_forward: level channel count does not match the config");
        ggml_tensor* layer = nn::conv2d(ctx, weights.projects[level].weight, weights.projects[level].bias,
                                       map, 1, 0);
        const ConvWeights& resize = weights.resizes[static_cast<size_t>(level)];
        if (resize.weight != nullptr) {
            const int kernel = static_cast<int>(resize.weight->ne[0]);
            if (kernel == 4) {
                layer = nn::conv_transpose2d(ctx, resize.weight, resize.bias, layer, 4);
            } else if (kernel == 2) {
                layer = nn::conv_transpose2d(ctx, resize.weight, resize.bias, layer, 2);
            } else {
                layer = nn::conv2d(ctx, resize.weight, resize.bias, layer, 2, 1);
            }
        }
        if (keep_stages) layer = session.carry(layer);
        output.layers[static_cast<size_t>(level)] = layer;
    }

    // Neck: 3x3 conv to `features` channels at each level.
    std::vector<ggml_tensor*> features(static_cast<size_t>(config.levels));
    for (int level = 0; level < config.levels; ++level) {
        features[static_cast<size_t>(level)] =
            nn::conv2d(ctx, weights.necks[level].weight, weights.necks[level].bias,
                       output.layers[static_cast<size_t>(level)], 1, 1);
    }

    // Feature fusion, deepest to shallowest. fusion[0] has no skip; each later
    // block adds the next shallower features and upsamples to the following
    // size. The last block upsamples x2 with no explicit size.
    ggml_tensor* path = features[static_cast<size_t>(config.levels - 1)];
    output.paths.resize(static_cast<size_t>(config.levels));
    for (int index = 0; index < config.levels; ++index) {
        const FusionWeights& fusion = weights.fusions[static_cast<size_t>(index)];
        if (index > 0) {
            const int skip_level = config.levels - 1 - index;
            ggml_tensor* skip = features[static_cast<size_t>(skip_level)];
            ggml_tensor* residual =
                residual_unit(ctx, fusion.res1_conv1, fusion.res1_conv2, skip);
            path = ggml_add(ctx, path, residual);
        }
        path = residual_unit(ctx, fusion.res2_conv1, fusion.res2_conv2, path);

        int64_t target_w = 0;
        int64_t target_h = 0;
        if (index == config.levels - 1) {
            target_w = 2 * features[0]->ne[0];
            target_h = 2 * features[0]->ne[1];
        } else {
            const ggml_tensor* next = features[static_cast<size_t>(config.levels - 2 - index)];
            target_w = next->ne[0];
            target_h = next->ne[1];
        }
        path = upsample_to(ctx, path, target_w, target_h);
        path = nn::conv2d(ctx, fusion.out_conv.weight, fusion.out_conv.bias, path, 1, 0);
        if (keep_stages) path = session.carry(path);
        output.paths[static_cast<size_t>(index)] = path;
    }

    ggml_tensor* head = nn::conv2d(ctx, weights.head_conv1.weight, weights.head_conv1.bias, path, 1, 1);
    if (keep_stages) head = session.carry(head);
    output.conv1 = head;  // 64 -> 32 channels at 1/3.5 of the input
    head = upsample_to(ctx, head, static_cast<int64_t>(config.patch_size) * patch_w,
                       static_cast<int64_t>(config.patch_size) * patch_h);

    head = nn::conv2d(ctx, weights.head_conv2.weight, weights.head_conv2.bias, head, 1, 1);
    head = ggml_relu(ctx, head);
    head = nn::conv2d(ctx, weights.head_conv3.weight, weights.head_conv3.bias, head, 1, 0);
    head = ggml_relu(ctx, head);
    output.depth = session.finish(head);
    return output;
}

FusionWeights resolve_fusion_block(const GgmlWeights& weights, const std::string& prefix) {
    FusionWeights fusion;
    fusion.res1_conv1 = resolve_conv(weights, prefix + ".resnet1.residual.1");
    fusion.res1_conv2 = resolve_conv(weights, prefix + ".resnet1.residual.3");
    fusion.res2_conv1 = resolve_conv(weights, prefix + ".resnet2.residual.1");
    fusion.res2_conv2 = resolve_conv(weights, prefix + ".resnet2.residual.3");
    fusion.out_conv = resolve_conv(weights, prefix + ".out_conv");
    if (weights.find(prefix + ".deconv.weight") != nullptr) {
        fusion.deconv = resolve_conv(weights, prefix + ".deconv");
    }
    return fusion;
}

ggml_tensor* fusion_block(ggml_context* ctx, const FusionWeights& weights, ggml_tensor* x,
                          ggml_tensor* skip, bool upsample) {
    require(x != nullptr, "fusion_block needs an input");
    if (skip != nullptr) {
        ggml_tensor* residual =
            residual_unit(ctx, weights.res1_conv1, weights.res1_conv2, skip);
        x = ggml_add(ctx, x, residual);
    }
    x = residual_unit(ctx, weights.res2_conv1, weights.res2_conv2, x);
    if (upsample) {
        require(weights.deconv.weight != nullptr, "fusion_block: this block has no deconv weight");
        x = nn::conv_transpose2d(ctx, weights.deconv.weight, weights.deconv.bias, x, 2);
    }
    return nn::conv2d(ctx, weights.out_conv.weight, weights.out_conv.bias, x, 1, 0);
}

}  // namespace splat
