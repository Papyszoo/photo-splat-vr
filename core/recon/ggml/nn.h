#pragma once

#include "ggml.h"

namespace splat {
namespace nn {

// Small, model-agnostic building blocks shared by the ViT (C3/C4 encoder) and
// the DPT head. Activations are channels-last (W, H, C, N) for convolutions and
// (C, N) for token tensors; the helpers keep that convention so a block can be
// lifted from DA2 into SHARP unchanged.

// Conv2d with bias, no padding groups. weight is ggml [KW, KH, Cin, Cout] and x
// is [W, H, Cin, N]; N > 1 runs the batch in one op.
ggml_tensor* conv2d(struct ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias, ggml_tensor* x,
                    int stride, int padding);
// ConvTranspose2d, stride-only (padding 0), weight ggml [KW, KH, Cout, Cin].
ggml_tensor* conv_transpose2d(struct ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias,
                              ggml_tensor* x, int stride);
// y = x @ weight^T + bias for a token tensor (C_in, N).
ggml_tensor* linear(struct ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias, ggml_tensor* x);
// LayerNorm over the innermost (channel) dimension.
ggml_tensor* layer_norm(struct ggml_context* ctx, ggml_tensor* x, ggml_tensor* weight,
                        ggml_tensor* bias, float eps);
// GroupNorm over `groups` contiguous blocks of the channel axis of a
// channels-last map, then the per-channel affine (PyTorch nn.GroupNorm). ggml's
// own op normalises ne[2] in contiguous groups, which is the same thing.
ggml_tensor* group_norm(struct ggml_context* ctx, ggml_tensor* x, ggml_tensor* weight,
                        ggml_tensor* bias, int groups, float eps);
// Adds a (C) bias to a channels-last tensor (W, H, C, N).
ggml_tensor* channel_bias(struct ggml_context* ctx, ggml_tensor* x, ggml_tensor* bias);
// Token tensor (C, N) -> channels-last map (W, H, C, 1); N must be W * H.
ggml_tensor* tokens_to_map(struct ggml_context* ctx, ggml_tensor* tokens, int width, int height);
// Channels-last map (W, H, C, N) -> token tensor (C, W*H) for one image, or
// (C, W*H, N) when the map carries a batch.
ggml_tensor* map_to_tokens(struct ggml_context* ctx, ggml_tensor* map);

}  // namespace nn
}  // namespace splat
