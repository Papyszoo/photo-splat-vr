#include "recon/ggml/vit.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "recon/ggml/check.h"
#include "recon/ggml/nn.h"
#include "recon/ggml/pos_embed.h"

namespace splat {
namespace {

std::string format_index(const std::string& pattern, int index) {
    const size_t position = pattern.find("%d");
    if (position == std::string::npos) return pattern;
    return pattern.substr(0, position) + std::to_string(index) + pattern.substr(position + 2);
}

std::string block_name(const ViTNames& names, int index, const std::string& field) {
    return format_index(names.block_prefix, index) + field + names.block_suffix;
}

// Slices one of the three (embed_dim, tokens) projections out of a fused
// qkv tensor and returns it as flash-attention Q/K/V: (head_dim, tokens, heads, 1).
ggml_tensor* attention_head_view(ggml_context* ctx, ggml_tensor* qkv, const ViTConfig& config,
                                 int which) {
    const int head_dim = config.embed_dim / config.heads;
    ggml_tensor* shaped = ggml_reshape_4d(ctx, qkv, head_dim, config.heads, 3, qkv->ne[1]);
    ggml_tensor* slice = ggml_view_4d(ctx, shaped, head_dim, config.heads, 1, qkv->ne[1],
                                      shaped->nb[1], shaped->nb[2], shaped->nb[3],
                                      static_cast<size_t>(which) * shaped->nb[2]);
    // (head_dim, heads, 1, tokens) -> (head_dim, tokens, heads, 1).
    // ggml_permute arguments map input axis i to output axis args[i].
    return ggml_cont(ctx, ggml_permute(ctx, slice, 0, 2, 3, 1));
}

ggml_tensor* project_heads(ggml_context* ctx, ggml_tensor* out, const ViTConfig& config) {
    // flash_attn result is (head_dim, heads, tokens, 1), already in the same
    // element order as the (embed_dim, tokens) projection input.
    return ggml_reshape_2d(ctx, out, config.embed_dim, out->ne[2]);
}

// Same for a batched qkv: slices one of the three projections out of the fused
// (3 * embed_dim, tokens, batch) tensor as (head_dim, tokens, heads, batch).
ggml_tensor* attention_head_view_batched(ggml_context* ctx, ggml_tensor* qkv,
                                         const ViTConfig& config, int which) {
    const int head_dim = config.embed_dim / config.heads;
    const int64_t tokens = qkv->ne[1];
    const int64_t batch = qkv->ne[2];
    // (3 * dim, tokens, batch) -> (dim, 3, tokens, batch): a pure reshape, the
    // fused rows are q, k, v in order.
    ggml_tensor* shaped = ggml_reshape_4d(ctx, qkv, config.embed_dim, 3, tokens, batch);
    ggml_tensor* heads = ggml_view_4d(ctx, shaped, head_dim, config.heads, tokens, batch,
                                      shaped->nb[0] * static_cast<size_t>(head_dim), shaped->nb[2],
                                      shaped->nb[3], static_cast<size_t>(which) * shaped->nb[1]);
    // (head_dim, heads, tokens, batch) -> (head_dim, tokens, heads, batch).
    return ggml_cont(ctx, ggml_permute(ctx, heads, 0, 2, 1, 3));
}

ggml_tensor* project_heads_batched(ggml_context* ctx, ggml_tensor* out, const ViTConfig& config) {
    // flash_attn result is (head_dim, heads, tokens, batch); the linear wants
    // (dim, tokens, batch), which is the same element order.
    return ggml_reshape_3d(ctx, out, config.embed_dim, out->ne[2], out->ne[3]);
}

ggml_tensor* separate_head(ggml_context* ctx, ggml_tensor* weight, ggml_tensor* bias,
                           ggml_tensor* tokens, const ViTConfig& config) {
    const int head_dim = config.embed_dim / config.heads;
    ggml_tensor* projected = nn::linear(ctx, weight, bias, tokens);
    ggml_tensor* shaped = ggml_reshape_4d(ctx, projected, head_dim, config.heads, 1, tokens->ne[1]);
    return ggml_cont(ctx, ggml_permute(ctx, shaped, 0, 2, 3, 1));
}

}  // namespace

ViTWeights resolve_vit_weights(const GgmlWeights& weights, const ViTConfig& config,
                               const ViTNames& names) {
    ViTWeights resolved;
    resolved.patch_weight = weights.need(names.prefix + names.patch_weight);
    resolved.patch_bias = weights.need(names.prefix + names.patch_bias);
    resolved.cls_token = weights.need(names.prefix + names.cls_token);
    resolved.pos_embed = weights.need(names.prefix + names.pos_embed);
    resolved.final_norm_weight = weights.need(names.prefix + names.final_norm_weight);
    resolved.final_norm_bias = weights.need(names.prefix + names.final_norm_bias);
    resolved.blocks.resize(static_cast<size_t>(config.blocks));
    for (int index = 0; index < config.blocks; ++index) {
        ViTBlockWeights& block = resolved.blocks[static_cast<size_t>(index)];
        auto get = [&](const std::string& field) {
            return weights.need(names.prefix + block_name(names, index, field));
        };
        block.ln1_weight = get(names.ln1_weight);
        block.ln1_bias = get(names.ln1_bias);
        block.ln2_weight = get(names.ln2_weight);
        block.ln2_bias = get(names.ln2_bias);
        if (config.combined_qkv) {
            block.qkv_weight = get(names.qkv_weight);
            block.qkv_bias = get(names.qkv_bias);
            if (block.qkv_weight->ne[1] != 3 * config.embed_dim) {
                throw std::runtime_error("ViT block " + std::to_string(index) + " qkv weight is " +
                                         std::to_string(block.qkv_weight->ne[1]) + " rows, expected " +
                                         std::to_string(3 * config.embed_dim));
            }
        } else {
            block.q_weight = get(names.q_weight);
            block.q_bias = get(names.q_bias);
            block.k_weight = get(names.k_weight);
            block.k_bias = get(names.k_bias);
            block.v_weight = get(names.v_weight);
            block.v_bias = get(names.v_bias);
        }
        block.proj_weight = get(names.proj_weight);
        block.proj_bias = get(names.proj_bias);
        block.fc1_weight = get(names.fc1_weight);
        block.fc1_bias = get(names.fc1_bias);
        block.fc2_weight = get(names.fc2_weight);
        block.fc2_bias = get(names.fc2_bias);
        const std::string ls1 = names.prefix + block_name(names, index, names.ls1);
        const std::string ls2 = names.prefix + block_name(names, index, names.ls2);
        block.ls1 = config.layer_scale ? weights.need(ls1) : weights.find(ls1);
        block.ls2 = config.layer_scale ? weights.need(ls2) : weights.find(ls2);
    }
    return resolved;
}

ggml_tensor* vit_patch_embed(GraphSession& session, const ViTWeights& weights,
                             const ViTConfig& config, ggml_tensor* image) {
    require(image != nullptr, "vit_patch_embed needs an image (W, H, 3, N)");
    require(image->ne[0] % config.patch_size == 0 && image->ne[1] % config.patch_size == 0,
            "image size is not a multiple of the patch size");
    ggml_context* ctx = session.ctx();
    // Native CONV_2D: an im2col buffer at this shape would stage the kernel in
    // fp16 (docs/research/op-audit.md §1.4) and the patch embed is cheap here.
    ggml_tensor* embedded = nn::conv2d(ctx, weights.patch_weight, weights.patch_bias, image,
                                       config.patch_size, 0);
    const int grid_w = static_cast<int>(embedded->ne[0]);
    const int grid_h = static_cast<int>(embedded->ne[1]);
    ggml_tensor* tokens = nn::map_to_tokens(ctx, embedded);
    require(tokens->ne[1] == static_cast<int64_t>(grid_w) * grid_h, "patch embed token count changed");
    return tokens;
}

ggml_tensor* vit_add_position(GraphSession& session, GgmlWeights& store,
                              const ViTWeights& weights, ggml_tensor* patch_tokens, int grid_w,
                              int grid_h, int out_w, int out_h, float interpolate_offset) {
    require(patch_tokens != nullptr, "vit_add_position needs patch tokens");
    const int dim = static_cast<int>(patch_tokens->ne[0]);
    const int64_t batch = patch_tokens->ne[2];
    const int64_t positions = weights.pos_embed->ne[1];
    require(positions == static_cast<int64_t>(grid_w) * grid_h + 1,
            "position embedding size does not match the configured grid");

    std::vector<float> pos_host;
    std::vector<float> cls_host;
    Status status = store.download(weights.pos_embed, &pos_host);
    if (!status.ok()) throw std::runtime_error(status.message());
    status = store.download(weights.cls_token, &cls_host);
    if (!status.ok()) throw std::runtime_error(status.message());

    const size_t rows = static_cast<size_t>(1 + out_w * out_h);
    ggml_tensor* position = session.persistent("pos_embed_interpolated",
                                               {dim, static_cast<int64_t>(rows)});
    if (out_w == grid_w && out_h == grid_h) {
        // timm adds the learned embedding directly when the token count
        // matches; resampling it here would shift the values by ~1e-4.
        if (pos_host.size() != rows * static_cast<size_t>(dim)) {
            throw std::runtime_error("position embedding has an unexpected size");
        }
        status = store.upload(position, pos_host.data(), rows * static_cast<size_t>(dim) * sizeof(float));
    } else {
        Result<std::vector<float>> resampled = resample_pos_embed(pos_host.data(), grid_w, grid_h,
                                                                 dim, out_w, out_h, interpolate_offset);
        if (!resampled.ok()) throw std::runtime_error(resampled.message());
        status = store.upload(position, resampled.value().data(),
                              rows * static_cast<size_t>(dim) * sizeof(float));
    }
    if (!status.ok()) throw std::runtime_error(status.message());

    ggml_context* ctx = session.ctx();
    ggml_tensor* cls = session.persistent("cls_token", {dim, 1});
    status = store.upload(cls, cls_host.data(), static_cast<size_t>(dim) * sizeof(float));
    if (!status.ok()) throw std::runtime_error(status.message());
    if (batch > 1) {
        // One cls token per item: repeat it so the concat sees equal batch axes.
        ggml_tensor* pattern = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, dim, 1, batch);
        cls = ggml_repeat(ctx, cls, pattern);
    }

    ggml_tensor* tokens = ggml_concat(ctx, cls, patch_tokens, 1);
    require(tokens != nullptr, "cannot concatenate the cls token");
    return ggml_add(ctx, tokens, position);
}

ggml_tensor* vit_block(GraphSession& session, const ViTBlockWeights& weights, const ViTConfig& config,
                       ggml_tensor* x) {
    require(x != nullptr && x->ne[0] == config.embed_dim, "vit_block input is not embed_dim wide");
    require(x->ne[3] == 1, "vit_block batches along ne[2], not ne[3]");
    ggml_context* ctx = session.ctx();
    const float scale = 1.0f / std::sqrt(static_cast<float>(config.embed_dim / config.heads));
    const bool batched = x->ne[2] > 1;

    ggml_tensor* normed = nn::layer_norm(ctx, x, weights.ln1_weight, weights.ln1_bias,
                                         config.norm_eps);
    ggml_tensor* q = nullptr;
    ggml_tensor* k = nullptr;
    ggml_tensor* v = nullptr;
    if (config.combined_qkv) {
        ggml_tensor* qkv = nn::linear(ctx, weights.qkv_weight, weights.qkv_bias, normed);
        if (batched) {
            q = attention_head_view_batched(ctx, qkv, config, 0);
            k = attention_head_view_batched(ctx, qkv, config, 1);
            v = attention_head_view_batched(ctx, qkv, config, 2);
        } else {
            q = attention_head_view(ctx, qkv, config, 0);
            k = attention_head_view(ctx, qkv, config, 1);
            v = attention_head_view(ctx, qkv, config, 2);
        }
    } else {
        require(!batched, "separate q/k/v weights are not used by the batched block");
        q = separate_head(ctx, weights.q_weight, weights.q_bias, normed, config);
        k = separate_head(ctx, weights.k_weight, weights.k_bias, normed, config);
        v = separate_head(ctx, weights.v_weight, weights.v_bias, normed, config);
    }

    ggml_tensor* attended = ggml_flash_attn_ext(ctx, q, k, v, nullptr, scale, 0.0f, 0.0f);
    require(attended != nullptr, "ggml_flash_attn_ext failed");
    ggml_tensor* tokens = batched ? project_heads_batched(ctx, attended, config)
                                  : project_heads(ctx, attended, config);
    ggml_tensor* projected = nn::linear(ctx, weights.proj_weight, weights.proj_bias, tokens);
    if (weights.ls1 != nullptr) projected = ggml_mul(ctx, projected, weights.ls1);
    ggml_tensor* residual = ggml_add(ctx, x, projected);

    ggml_tensor* mlp_in = nn::layer_norm(ctx, residual, weights.ln2_weight, weights.ln2_bias,
                                        config.norm_eps);
    ggml_tensor* hidden = nn::linear(ctx, weights.fc1_weight, weights.fc1_bias, mlp_in);
    hidden = ggml_gelu_erf(ctx, hidden);
    ggml_tensor* mlp_out = nn::linear(ctx, weights.fc2_weight, weights.fc2_bias, hidden);
    if (weights.ls2 != nullptr) mlp_out = ggml_mul(ctx, mlp_out, weights.ls2);
    ggml_tensor* out = ggml_add(ctx, residual, mlp_out);
    return session.checkpoint(out);
}

ggml_tensor* vit_final_norm(GraphSession& session, const ViTWeights& weights, float eps,
                            ggml_tensor* x) {
    return nn::layer_norm(session.ctx(), x, weights.final_norm_weight, weights.final_norm_bias, eps);
}

}  // namespace splat
