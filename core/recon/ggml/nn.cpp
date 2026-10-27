#include "recon/ggml/nn.h"

#include <stdexcept>
#include <string>

#include "recon/ggml/check.h"

namespace splat {
namespace nn {

ggml_tensor* conv2d(ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias, ggml_tensor* x,
                    int stride, int padding) {
    require(weight != nullptr && x != nullptr, "conv2d needs a weight and an input");
    require(weight->ne[2] == x->ne[2], "conv2d kernel/input channel mismatch");
    ggml_tensor* out = ggml_conv_2d_direct(ctx, weight, x, stride, stride, padding, padding, 1, 1);
    require(out != nullptr, "ggml_conv_2d_direct failed");
    return bias == nullptr ? out : channel_bias(ctx, out, bias);
}

ggml_tensor* conv_transpose2d(ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias,
                              ggml_tensor* x, int stride) {
    require(weight != nullptr && x != nullptr, "conv_transpose2d needs a weight and an input");
    ggml_tensor* out = ggml_conv_transpose_2d_p0(ctx, weight, x, stride);
    require(out != nullptr, "ggml_conv_transpose_2d_p0 failed");
    return bias == nullptr ? out : channel_bias(ctx, out, bias);
}

ggml_tensor* linear(ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias, ggml_tensor* x) {
    require(weight != nullptr && x != nullptr, "linear needs a weight and an input");
    require(weight->ne[0] == x->ne[0], "linear weight/input feature mismatch");
    ggml_tensor* out = ggml_mul_mat(ctx, weight, x);
    // fp16 weights are allowed (the SHARP GGUF is an fp16 file), but the
    // multiply must not be downcast to fp16 as well: ggml-vulkan's default
    // matmul for non-F32 weights accumulates in fp16 and, with fp16 weights,
    // also converts the activations to fp16 ("prefer_f16_b"). Over 24 ViT
    // blocks that drags the stage cosine from 0.99999 to 0.999. Ask for f32
    // activations and f32 accumulation explicitly; the cost is Vulkan matmul
    // throughput, not correctness, and the spec's risk row already says
    // numerically sensitive layers stay fp32.
    ggml_prec_set_src(out, GGML_PREC_F32, 1);
    ggml_prec_set_acc(out, GGML_PREC_F32);
    if (bias == nullptr) return out;
    // bias is (C_out, 1) in ggml order, so it repeats over the token dimension.
    return ggml_add(ctx, out, bias);
}

ggml_tensor* layer_norm(ggml_context* ctx, ggml_tensor* x, ggml_tensor* weight, ggml_tensor* bias,
                        float eps) {
    ggml_tensor* normed = ggml_norm(ctx, x, eps);
    if (weight != nullptr) normed = ggml_mul(ctx, normed, weight);
    if (bias != nullptr) normed = ggml_add(ctx, normed, bias);
    return normed;
}

ggml_tensor* group_norm(ggml_context* ctx, ggml_tensor* x, ggml_tensor* weight, ggml_tensor* bias,
                        int groups, float eps) {
    require(x != nullptr && weight != nullptr, "group_norm needs an input and a weight");
    require(x->ne[2] % groups == 0, "group_norm: channel count is not a multiple of the group count");
    ggml_tensor* normed = ggml_group_norm(ctx, x, groups, eps);
    require(normed != nullptr, "ggml_group_norm failed");
    require(weight->ne[0] == x->ne[2], "group_norm: weight length does not match the channel count");
    normed = ggml_mul(ctx, normed, ggml_reshape_4d(ctx, weight, 1, 1, x->ne[2], 1));
    if (bias == nullptr) return normed;
    return channel_bias(ctx, normed, bias);
}

ggml_tensor* channel_bias(ggml_context* ctx, ggml_tensor* x, ggml_tensor* bias) {
    require(bias != nullptr, "channel_bias needs a bias");
    const int64_t channels = bias->ne[0];
    require(x->ne[2] == channels, "channel_bias: bias length does not match the channel count");
    ggml_tensor* shaped = ggml_reshape_4d(ctx, bias, 1, 1, channels, 1);
    return ggml_add(ctx, x, shaped);
}

ggml_tensor* tokens_to_map(ggml_context* ctx, ggml_tensor* tokens, int width, int height) {
    require(tokens != nullptr && tokens->ne[1] == static_cast<int64_t>(width) * height,
            "tokens_to_map: token count does not match width * height");
    ggml_tensor* channels_last = ggml_cont(ctx, ggml_permute(ctx, tokens, 1, 0, 2, 3));
    return ggml_reshape_4d(ctx, channels_last, width, height, tokens->ne[0], 1);
}

ggml_tensor* map_to_tokens(ggml_context* ctx, ggml_tensor* map) {
    require(map != nullptr, "map_to_tokens needs a channels-last map");
    // (W, H, C, N) -> (C, W*H, N): the permute leaves the channel axis
    // outermost; the reshape is then a pure reinterpretation.
    ggml_tensor* transposed = ggml_cont(ctx, ggml_permute(ctx, map, 1, 2, 0, 3));
    if (map->ne[3] > 1) {
        return ggml_reshape_3d(ctx, transposed, map->ne[2], map->ne[0] * map->ne[1], map->ne[3]);
    }
    return ggml_reshape_2d(ctx, transposed, map->ne[2], map->ne[0] * map->ne[1]);
}

}  // namespace nn
}  // namespace splat
