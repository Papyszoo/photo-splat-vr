#pragma once

#include <string>
#include <vector>

#include "ggml.h"
#include "recon/ggml/graph.h"
#include "recon/ggml/weights.h"

namespace splat {

// Configurable DINOv2-style ViT encoder, shared by the C4 fallback (ViT-S/14)
// and the C3 SHARP port (ViT-L/16, DINOv2-style with LayerScale). Nothing here
// is DA2-specific: shapes, depth, heads, tap layers, the patch grid and the
// GGUF tensor names all come from the config, so SHARP reuses the same block by
// passing its timm names (fused qkv, "ls1.gamma", ...) and 24 layers.
struct ViTConfig {
    int embed_dim = 384;
    int heads = 6;
    int blocks = 12;
    int patch_size = 14;
    int mlp_hidden = 1536;
    float norm_eps = 1e-6f;
    // timm/DINOv2 checkpoints store one fused (3*dim, dim) qkv projection;
    // the HF Depth Anything export stores q, k and v separately.
    bool combined_qkv = true;
    // timm's `init_values` checkpoints carry LayerScale (ls1.gamma/ls2.gamma).
    // The HF Depth Anything export has none and the reference block then skips
    // the multiply; when this is true the tensors are required, so a missing
    // LayerScale is a load error instead of a silently wrong graph.
    bool layer_scale = false;
    // Block indices whose output feeds the dense head (0-based, as in
    // DINOv2.get_intermediate_layers).
    std::vector<int> taps{2, 5, 8, 11};
};

// GGUF tensor names for one ViT. `%d` is replaced by the block index.
// `prefix` is prepended to every name (SHARP stores the two ViTs under
// "encoder.patch_encoder." and "encoder.image_encoder."); the DA2 profile
// leaves it empty.
struct ViTNames {
    std::string prefix = "";
    std::string patch_weight = "patch_embed.proj.weight";
    std::string patch_bias = "patch_embed.proj.bias";
    std::string cls_token = "cls_token";
    std::string pos_embed = "pos_embed";
    std::string final_norm_weight = "norm.weight";
    std::string final_norm_bias = "norm.bias";
    std::string block_prefix = "blocks.%d.";
    std::string block_suffix = "";
    std::string ln1_weight = "norm1.weight";
    std::string ln1_bias = "norm1.bias";
    std::string ln2_weight = "norm2.weight";
    std::string ln2_bias = "norm2.bias";
    std::string qkv_weight = "attn.qkv.weight";
    std::string qkv_bias = "attn.qkv.bias";
    std::string q_weight = "attn.q.weight";
    std::string q_bias = "attn.q.bias";
    std::string k_weight = "attn.k.weight";
    std::string k_bias = "attn.k.bias";
    std::string v_weight = "attn.v.weight";
    std::string v_bias = "attn.v.bias";
    std::string proj_weight = "attn.proj.weight";
    std::string proj_bias = "attn.proj.bias";
    std::string fc1_weight = "mlp.fc1.weight";
    std::string fc1_bias = "mlp.fc1.bias";
    std::string fc2_weight = "mlp.fc2.weight";
    std::string fc2_bias = "mlp.fc2.bias";
    // timm names the LayerScale parameter ls1.gamma; the Depth Anything export
    // calls it ls1. Override in the model that needs the other spelling.
    std::string ls1 = "ls1";
    std::string ls2 = "ls2";
};

struct ViTBlockWeights {
    ggml_tensor* ln1_weight = nullptr;
    ggml_tensor* ln1_bias = nullptr;
    ggml_tensor* ln2_weight = nullptr;
    ggml_tensor* ln2_bias = nullptr;
    ggml_tensor* qkv_weight = nullptr;
    ggml_tensor* qkv_bias = nullptr;
    ggml_tensor* q_weight = nullptr;
    ggml_tensor* q_bias = nullptr;
    ggml_tensor* k_weight = nullptr;
    ggml_tensor* k_bias = nullptr;
    ggml_tensor* v_weight = nullptr;
    ggml_tensor* v_bias = nullptr;
    ggml_tensor* proj_weight = nullptr;
    ggml_tensor* proj_bias = nullptr;
    ggml_tensor* fc1_weight = nullptr;
    ggml_tensor* fc1_bias = nullptr;
    ggml_tensor* fc2_weight = nullptr;
    ggml_tensor* fc2_bias = nullptr;
    ggml_tensor* ls1 = nullptr;
    ggml_tensor* ls2 = nullptr;
};

struct ViTWeights {
    ggml_tensor* patch_weight = nullptr;
    ggml_tensor* patch_bias = nullptr;
    ggml_tensor* cls_token = nullptr;
    ggml_tensor* pos_embed = nullptr;
    ggml_tensor* final_norm_weight = nullptr;
    ggml_tensor* final_norm_bias = nullptr;
    std::vector<ViTBlockWeights> blocks;
};

// Resolves every tensor the config names. Throws std::runtime_error when the
// GGUF does not match the config.
ViTWeights resolve_vit_weights(const GgmlWeights& weights, const ViTConfig& config,
                               const ViTNames& names);

// Patch embedding: (W, H, 3, 1) image -> (embed_dim, (W/patch)*(H/patch)).
// With N images in ne[3] the batch runs in one conv and the tokens come back as
// (embed_dim, patches, N), which is how SHARP encodes its 35 pyramid patches.
ggml_tensor* vit_patch_embed(GraphSession& session, const ViTWeights& weights,
                             const ViTConfig& config, ggml_tensor* image);

// Adds cls + interpolated position embedding. `grid_w`, `grid_h` are the
// checkpoint's position grid (37x37 for DINOv2 at 518); `out_w`, `out_h` the
// patch grid of this input. Returns (embed_dim, 1 + out_w*out_h) with cls first
// (plus the batch axis when the tokens carry one). At the checkpoint's own grid
// the learned embedding is added directly, as timm does, with no resample.
ggml_tensor* vit_add_position(GraphSession& session, GgmlWeights& store,
                              const ViTWeights& weights, ggml_tensor* patch_tokens, int grid_w,
                              int grid_h, int out_w, int out_h, float interpolate_offset = 0.1f);

// One DINOv2/timm block: LN -> attention -> LayerScale -> residual, then
// LN -> MLP (GELU erf) -> LayerScale -> residual. Submits along the way so no
// single GPU submission grows unbounded. Accepts (dim, tokens) or
// (dim, tokens, batch) token tensors.
ggml_tensor* vit_block(GraphSession& session, const ViTBlockWeights& weights, const ViTConfig& config,
                       ggml_tensor* x);

// Final LayerNorm over the channel dimension (applied to tap tensors).
ggml_tensor* vit_final_norm(GraphSession& session, const ViTWeights& weights, float eps,
                            ggml_tensor* x);

}  // namespace splat
