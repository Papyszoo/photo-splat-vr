#include "recon/sharp/sharp_encoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "recon/ggml/check.h"
#include "recon/ggml/nn.h"
#include "recon/sharp/sharp_preprocess.h"

namespace splat {
namespace {

constexpr int kEmbedDim = 1024;
constexpr int kHeads = 16;
constexpr int kBlocks = 24;
constexpr int kPatchPixels = 16;  // ViT patch size
// dims_encoder = [last_encoder=256] + MONODEPTH_ENCODER_DIMS_MAP["dinov2l16_384"].
constexpr int kLevelChannels[5] = {256, 256, 512, 1024, 1024};
// merge(padding=3) trims the interior tile edges of the 24x24 token grid.
constexpr int kMergePadding = 3;

double seconds_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

// The reference's trimmed sliding-window merge (spn_encoder.merge) written out
// once, so both the host merge and the tests use the same geometry.
struct MergeGeometry {
    int steps = 1;
    int padding = 0;
    int patch = 24;   // token grid of one patch
    int out = 24;     // merged token grid
};

MergeGeometry merge_geometry(int steps, int padding, int patch) {
    MergeGeometry geometry;
    geometry.steps = steps;
    geometry.padding = padding;
    geometry.patch = patch;
    // Interior tiles lose `padding` on both sides, the border tiles on one.
    geometry.out = steps * patch - 2 * padding * (steps - 1);
    return geometry;
}

// Placement of tile `index` (row or column) inside the merged grid: the sum of
// the trimmed lengths of the tiles before it.
void tile_span(const MergeGeometry& geometry, int index, int* source, int* destination, int* length) {
    *destination = 0;
    for (int previous = 0; previous < index; ++previous) {
        *destination += geometry.patch - (previous > 0 ? geometry.padding : 0) -
                        (previous < geometry.steps - 1 ? geometry.padding : 0);
    }
    const int trim_before = index > 0 ? geometry.padding : 0;
    const int trim_after = index < geometry.steps - 1 ? geometry.padding : 0;
    *source = trim_before;
    *length = geometry.patch - trim_before - trim_after;
}

// Accumulates one patch's (patch, patch, dim) token map into a merged (out, out,
// dim) host map, x fastest then y then channel. Returns the pixel count copied.
int merge_patch_into(const MergeGeometry& geometry, int dim, const float* map, int tile_x,
                     int tile_y, float* out) {
    int source_x = 0, destination_x = 0, length_x = 0;
    int source_y = 0, destination_y = 0, length_y = 0;
    tile_span(geometry, tile_x, &source_x, &destination_x, &length_x);
    tile_span(geometry, tile_y, &source_y, &destination_y, &length_y);
    for (int channel = 0; channel < dim; ++channel) {
        const float* source = map + static_cast<size_t>(channel) * geometry.patch * geometry.patch;
        float* destination = out + static_cast<size_t>(channel) * geometry.out * geometry.out;
        for (int y = 0; y < length_y; ++y) {
            std::memcpy(destination + static_cast<size_t>(destination_y + y) * geometry.out +
                            destination_x,
                        source + static_cast<size_t>(source_y + y) * geometry.patch + source_x,
                        static_cast<size_t>(length_x) * sizeof(float));
        }
    }
    return length_x * length_y;
}

// Per-patch token map (dim, 1 + patch*patch, batch) -> (patch, patch, dim) for
// one item, dropping the cls token. Channel-outer like the ggml tap tensor, so
// each (channel, row) copy is contiguous.
void patch_map_from_tokens(const float* tokens, int dim, int patch, int item, float* out) {
    const int columns = 1 + patch * patch;
    for (int channel = 0; channel < dim; ++channel) {
        float* destination = out + static_cast<size_t>(channel) * patch * patch;
        for (int y = 0; y < patch; ++y) {
            const float* source =
                tokens + channel + static_cast<size_t>(dim) * ((1 + y * patch) + columns * item);
            for (int x = 0; x < patch; ++x) {
                destination[y * patch + x] = source[static_cast<size_t>(dim) * x];
            }
        }
    }
}

}  // namespace

SharpEncoder::~SharpEncoder() = default;

Status SharpEncoder::initialize(std::shared_ptr<SharpModel> model) {
    model_ = std::move(model);
    GgmlWeights& weights = model_->weights();

    vit_config_ = ViTConfig{};
    vit_config_.embed_dim = kEmbedDim;
    vit_config_.heads = kHeads;
    vit_config_.blocks = kBlocks;
    vit_config_.patch_size = kPatchPixels;
    vit_config_.mlp_hidden = 4 * kEmbedDim;
    vit_config_.norm_eps = 1e-6f;
    vit_config_.combined_qkv = true;
    vit_config_.layer_scale = true;
    // 5 and 11 feed upsample_latent0/1; the reference also registers 17 and 23
    // but never consumes them (x0/x1/x2 come from the final normed output).
    vit_config_.taps = {5, 11};
    if (config_.patch_batch < 1) config_.patch_batch = 1;
    patch_names_ = ViTNames{};
    patch_names_.prefix = "encoder.patch_encoder.";
    patch_names_.ls1 = "ls1.gamma";
    patch_names_.ls2 = "ls2.gamma";
    image_names_ = ViTNames{};
    image_names_.prefix = "encoder.image_encoder.";
    image_names_.ls1 = "ls1.gamma";
    image_names_.ls2 = "ls2.gamma";

    try {
        patch_weights_ = resolve_vit_weights(weights, vit_config_, patch_names_);
        image_weights_ = resolve_vit_weights(weights, vit_config_, image_names_);
        auto resolve_upsampler = [&weights](const std::string& name) {
            Upsampler upsampler;
            for (int index = 0;; ++index) {
                ggml_tensor* weight =
                    weights.find(name + "." + std::to_string(index) + ".weight");
                if (weight == nullptr) break;
                upsampler.weights.push_back(weight);
            }
            if (upsampler.weights.empty()) {
                throw std::runtime_error("missing GGUF tensor '" + name + ".0.weight'");
            }
            return upsampler;
        };
        latent0_ = resolve_upsampler("encoder.upsample_latent0");
        latent1_ = resolve_upsampler("encoder.upsample_latent1");
        up0_ = resolve_upsampler("encoder.upsample0");
        up1_ = resolve_upsampler("encoder.upsample1");
        up2_ = resolve_upsampler("encoder.upsample2");
        up2_.lowres_weight = weights.need("encoder.upsample_lowres.weight");
        up2_.lowres_bias = weights.need("encoder.upsample_lowres.bias");
        up2_.fuse_weight = weights.need("encoder.fuse_lowres.weight");
        up2_.fuse_bias = weights.need("encoder.fuse_lowres.bias");
    } catch (const std::exception& error) {
        return Status::error(std::string("GGUF does not match the SHARP encoder graph: ") + error.what());
    }

    // Shape checks: the config is hard-coded, so a mismatched file must be
    // rejected here rather than producing a plausible-looking wrong result.
    const ggml_tensor* pos = patch_weights_.pos_embed;
    const int grid = static_cast<int>(std::lround(std::sqrt(static_cast<double>(pos->ne[1] - 1))));
    if (static_cast<int64_t>(grid) * grid != pos->ne[1] - 1 ||
        pos->ne[0] != kEmbedDim ||
        grid * kPatchPixels != config_.patch_size) {
        return Status::error("patch-encoder position embedding does not match a 384x384 ViT-L/16");
    }
    if (patch_weights_.blocks[0].qkv_weight->ne[1] != 3 * kEmbedDim ||
        latent0_.weights.size() != 4 || latent1_.weights.size() != 3 ||
        up0_.weights.size() != 2 || up1_.weights.size() != 2 || up2_.weights.size() != 2) {
        return Status::error("GGUF does not match the SHARP encoder graph (qkv / upsampler shape)");
    }
    return Status::success();
}

Result<std::unique_ptr<SharpEncoder>> SharpEncoder::create(const Path& gguf, bool prefer_vulkan,
                                                          SharpEncoderConfig config) {
    using ResultT = Result<std::unique_ptr<SharpEncoder>>;
    Result<std::shared_ptr<SharpModel>> model = SharpModel::load(gguf, prefer_vulkan);
    if (!model.ok()) return ResultT::failure(model.message());
    return create(std::move(model.value()), config);
}

Result<std::unique_ptr<SharpEncoder>> SharpEncoder::create(std::shared_ptr<SharpModel> model,
                                                          SharpEncoderConfig config) {
    using ResultT = Result<std::unique_ptr<SharpEncoder>>;
    auto encoder = std::unique_ptr<SharpEncoder>(new SharpEncoder());
    encoder->config_ = config;
    const Status status = encoder->initialize(std::move(model));
    if (!status.ok()) return ResultT::failure(status.message());
    return ResultT::success(std::move(encoder));
}

ggml_tensor* SharpEncoder::level(int index) const {
    if (index < 0 || index >= static_cast<int>(levels_.size())) return nullptr;
    return levels_[static_cast<size_t>(index)];
}

Result<SharpEncoderResult> SharpEncoder::run(const float* input_chw, int image_size,
                                             const SharpEncoderStageCallback& stage_cb,
                                             Progress* progress) {
    using ResultT = Result<SharpEncoderResult>;
    const auto started = std::chrono::steady_clock::now();
    if (input_chw == nullptr) return ResultT::failure("run() got a null input");
    if (image_size != config_.image_size || image_size != 4 * config_.patch_size) {
        return ResultT::failure("SHARP's encoder expects " + std::to_string(4 * config_.patch_size) +
                                " px, got " + std::to_string(image_size));
    }
    const int patch = config_.patch_size;
    const int patch_pixels = patch / kPatchPixels;  // 24 tokens per side

    try {
        levels_.fill(nullptr);
        session_.reset();
        SharpEncoderResult result;
        result.image_size = image_size;
        for (int level = 0; level < 5; ++level) {
            result.level_channels[static_cast<size_t>(level)] = kLevelChannels[level];
        }

        if (progress != nullptr) progress->report(0.02f, "pyramid");
        auto mark = std::chrono::steady_clock::now();
        Result<SharpPatches> patches = sharp_pyramid_patches(input_chw, image_size, patch);
        if (!patches.ok()) return ResultT::failure(patches.message());
        result.pyramid_seconds = seconds_since(mark);

        const std::vector<int>& steps = patches.value().steps;

        // ---- patch encoder, `patch_batch` patches per submit --------------
        // The five merged maps: block 5 and block 11 over the 5x5 level (96x96
        // each), and the final normed output over 5x5 (96x96), 3x3 (48x48) and
        // the single 384x384 patch (24x24). Merging happens on the host because
        // the trimmed sliding window is a gather that no ggml op expresses
        // without index tensors (docs/research/op-audit.md §3.3); the taps leave
        // the graph as fp32 either way.
        struct MergeTarget {
            MergeGeometry geometry;
            std::vector<float> values;
            int written = 0;   // pixels merged so far, checked against the area
        };
        MergeTarget merged[5];
        merged[0].geometry = merge_geometry(steps[0], kMergePadding, patch_pixels);
        merged[1].geometry = merged[0].geometry;
        merged[2].geometry = merged[0].geometry;
        merged[3].geometry = merge_geometry(steps[1], 2 * kMergePadding, patch_pixels);
        merged[4].geometry = merge_geometry(steps[2], 0, patch_pixels);
        for (MergeTarget& target : merged) {
            const int size = target.geometry.out;
            target.values.assign(static_cast<size_t>(size) * size * kEmbedDim, 0.0f);
        }

        mark = std::chrono::steady_clock::now();
        // Patch index ranges of each level, and which merged map each tap goes
        // to. Level 0 is patches [0, 25), level 1 [25, 34), level 2 patch 34.
        const int level0_end = steps[0] * steps[0];
        const int level1_end = level0_end + steps[1] * steps[1];
        std::vector<float> tap_pixels(static_cast<size_t>(kEmbedDim) * patch_pixels * patch_pixels);

        std::vector<float> tap;      // one downloaded tap tensor
        size_t patch_submits = 0;    // the patch encoder's own sessions
        for (int begin = 0; begin < patches.value().count; begin += config_.patch_batch) {
            const int count = std::min(config_.patch_batch, patches.value().count - begin);
            GraphSession batch(model_->backend(), model_->weights(), config_.nodes_per_submit,
                               config_.batch_arena_bytes);
            ggml_tensor* image = batch.persistent(
                "sharp_patches", {patch, patch, 3, count});
            Status uploaded = model_->weights().upload(
                image,
                patches.value().values.data() + static_cast<size_t>(begin) * 3 * patch * patch,
                static_cast<size_t>(count) * 3 * patch * patch * sizeof(float));
            if (!uploaded.ok()) return ResultT::failure(uploaded.message());

            ggml_tensor* tokens = vit_patch_embed(batch, patch_weights_, vit_config_, image);
            tokens = vit_add_position(batch, model_->weights(), patch_weights_, tokens, patch_pixels,
                                      patch_pixels, patch_pixels, patch_pixels);

            // Taps 5 and 11 plus the final normed output. Every tap is carried
            // (so it survives the next submit) and read back for the merge.
            for (int block = 0; block < kBlocks; ++block) {
                tokens = vit_block(batch, patch_weights_.blocks[static_cast<size_t>(block)],
                                   vit_config_, tokens);
                const int index = block == 5 ? 0 : (block == 11 ? 1 : -1);
                if (index < 0) continue;
                // carry() submits and returns the copy that survives the next
                // submit; the pre-carry tensor's data is gone from here on.
                tokens = batch.carry(tokens);
                const Status status = batch.download(tokens, &tap);
                if (!status.ok()) return ResultT::failure(status.message());
                MergeTarget& target = merged[static_cast<size_t>(index)];
                for (int item = 0; item < count; ++item) {
                    const int patch_index = begin + item;
                    if (patch_index >= level0_end) continue;
                    patch_map_from_tokens(tap.data(), kEmbedDim, patch_pixels, item, tap_pixels.data());
                    target.written += merge_patch_into(target.geometry, kEmbedDim,
                                                       tap_pixels.data(),
                                                       patch_index % steps[0],
                                                       patch_index / steps[0], target.values.data());
                }
            }
            ggml_tensor* normed = vit_final_norm(batch, patch_weights_, vit_config_.norm_eps, tokens);
            ggml_tensor* final_tokens = batch.carry(normed);
            const Status status = batch.download(final_tokens, &tap);
            if (!status.ok()) return ResultT::failure(status.message());
            for (int item = 0; item < count; ++item) {
                const int patch_index = begin + item;
                patch_map_from_tokens(tap.data(), kEmbedDim, patch_pixels, item, tap_pixels.data());
                if (patch_index < level0_end) {
                    MergeTarget& target = merged[2];
                    target.written += merge_patch_into(target.geometry, kEmbedDim,
                                                       tap_pixels.data(), patch_index % steps[0],
                                                       patch_index / steps[0], target.values.data());
                } else if (patch_index < level1_end) {
                    MergeTarget& target = merged[3];
                    const int local = patch_index - level0_end;
                    target.written += merge_patch_into(target.geometry, kEmbedDim,
                                                       tap_pixels.data(), local % steps[1],
                                                       local / steps[1], target.values.data());
                } else {
                    MergeTarget& target = merged[4];
                    target.written += merge_patch_into(target.geometry, kEmbedDim,
                                                       tap_pixels.data(), 0, 0, target.values.data());
                }
            }
            if (progress != nullptr) {
                progress->report(0.02f + 0.6f * static_cast<float>(begin + count) /
                                             static_cast<float>(patches.value().count),
                                 "patch encoder");
            }
            patch_submits += batch.submits();
        }
        result.patch_encoder_seconds = seconds_since(mark);

        // Every output pixel must be covered exactly once; a shortfall means the
        // patch geometry and the merge disagree.
        for (int index = 0; index < 5; ++index) {
            const MergeTarget& target = merged[static_cast<size_t>(index)];
            const int area = target.geometry.out * target.geometry.out;
            if (target.written != area) {
                return ResultT::failure("sliding-window merge left " +
                                        std::to_string(area - target.written) + " pixels of map " +
                                        std::to_string(index) + " uncovered");
            }
        }

        // ---- merged maps -> arena ----------------------------------------
        mark = std::chrono::steady_clock::now();
        session_ = std::make_unique<GraphSession>(model_->backend(), model_->weights(), config_.nodes_per_submit,
                                                  config_.arena_bytes);
        GraphSession& session = *session_;
        std::array<ggml_tensor*, 5> maps{{nullptr, nullptr, nullptr, nullptr, nullptr}};
        for (int index = 0; index < 5; ++index) {
            const MergeTarget& target = merged[static_cast<size_t>(index)];
            const int size = target.geometry.out;
            char name[32];
            std::snprintf(name, sizeof(name), "merged_%02d", index);
            maps[static_cast<size_t>(index)] =
                session.persistent(name, {size, size, kEmbedDim, 1});
            const Status status = model_->weights().upload(maps[static_cast<size_t>(index)],
                                                  target.values.data(),
                                                  target.values.size() * sizeof(float));
            if (!status.ok()) return ResultT::failure(status.message());
        }
        if (stage_cb) {
            for (int index = 0; index < 5; ++index) {
                SharpEncoderStage stage;
                char stage_name[32];
                std::snprintf(stage_name, sizeof(stage_name), "merged_%02d", index);
                stage.name = stage_name;
                const ggml_tensor* tensor = maps[static_cast<size_t>(index)];
                stage.shape = {tensor->ne[0], tensor->ne[1], tensor->ne[2], tensor->ne[3]};
                const Status status = session.download(tensor, &stage.values);
                if (!status.ok()) return ResultT::failure(status.message());
                stage_cb(stage);
            }
        }
        result.merge_seconds = seconds_since(mark);
        std::vector<float>().swap(tap);
        std::vector<float>().swap(tap_pixels);
        for (MergeTarget& target : merged) std::vector<float>().swap(target.values);

        // ---- upsamplers ---------------------------------------------------
        mark = std::chrono::steady_clock::now();
        // Every apply() ends in carry(), which submits the segment and opens a
        // new context; each op must therefore be built in the *current* one
        // (session.ctx()), never in a context captured before the carry.
        auto apply = [&](const Upsampler& upsampler, ggml_tensor* map) {
            ggml_context* ctx = session.ctx();
            ggml_tensor* x = nn::conv2d(ctx, upsampler.weights[0], nullptr, map, 1, 0);
            for (size_t index = 1; index < upsampler.weights.size(); ++index) {
                x = nn::conv_transpose2d(ctx, upsampler.weights[index], nullptr, x, 2);
            }
            return session.carry(x);
        };
        levels_[0] = apply(latent0_, maps[0]);
        levels_[1] = apply(latent1_, maps[1]);
        levels_[2] = apply(up0_, maps[2]);
        levels_[3] = apply(up1_, maps[3]);
        ggml_tensor* lowres_level = apply(up2_, maps[4]);

        // ---- image encoder on the 384x384 level ---------------------------
        mark = std::chrono::steady_clock::now();
        ggml_tensor* lowres_image = session.persistent("sharp_lowres", {patch, patch, 3, 1});
        Status uploaded = model_->weights().upload(lowres_image, patches.value().lowres.data(),
                                          static_cast<size_t>(3) * patch * patch * sizeof(float));
        if (!uploaded.ok()) return ResultT::failure(uploaded.message());
        ggml_tensor* image_tokens = vit_patch_embed(session, image_weights_, vit_config_, lowres_image);
        image_tokens = vit_add_position(session, model_->weights(), image_weights_, image_tokens,
                                        patch_pixels, patch_pixels, patch_pixels, patch_pixels);
        for (int block = 0; block < kBlocks; ++block) {
            image_tokens = vit_block(session, image_weights_.blocks[static_cast<size_t>(block)],
                                     vit_config_, image_tokens);
        }
        image_tokens = vit_final_norm(session, image_weights_, vit_config_.norm_eps, image_tokens);
        // The view is built in the segment that produced image_tokens; the
        // carry after it opens a new one for the ops below.
        ggml_tensor* image_patches = ggml_view_2d(
            session.ctx(), image_tokens, kEmbedDim, patch_pixels * patch_pixels,
            image_tokens->nb[1], static_cast<size_t>(kEmbedDim) * sizeof(float));
        image_patches = session.carry(image_patches);
        ggml_context* ctx = session.ctx();
        ggml_tensor* image_map = nn::tokens_to_map(ctx, image_patches, patch_pixels, patch_pixels);
        ggml_tensor* image_lowres = nn::conv_transpose2d(ctx, up2_.lowres_weight, up2_.lowres_bias,
                                                         image_map, 2);
        image_lowres = session.carry(image_lowres);
        result.image_encoder_seconds = seconds_since(mark);

        // fuse_lowres: cat([x2_features, lowres], dim=1) -> 1x1 conv.
        ggml_context* fuse_ctx = session.ctx();
        ggml_tensor* concatenated = ggml_concat(fuse_ctx, lowres_level, image_lowres, 2);
        require(concatenated != nullptr && concatenated->ne[2] == 2 * kLevelChannels[4],
                "fuse_lowres expects 2048 input channels");
        levels_[4] = session.carry(nn::conv2d(fuse_ctx, up2_.fuse_weight, up2_.fuse_bias,
                                              concatenated, 1, 0));
        result.upsample_seconds = seconds_since(mark);

        // ---- report -------------------------------------------------------
        for (int index = 0; index < 5; ++index) {
            const ggml_tensor* tensor = levels_[static_cast<size_t>(index)];
            require(tensor != nullptr, "a level was not produced");
            result.level_sizes[static_cast<size_t>(index)] = {
                static_cast<int>(tensor->ne[0]), static_cast<int>(tensor->ne[1])};
            if (stage_cb) {
                SharpEncoderStage stage;
                stage.name = "encoder_out_0" + std::to_string(index);
                stage.shape = {tensor->ne[0], tensor->ne[1], tensor->ne[2], tensor->ne[3]};
                const Status status = session.download(tensor, &stage.values);
                if (!status.ok()) return ResultT::failure(status.message());
                stage_cb(stage);
            }
        }
        // Every submit of the run, not just the post-merge session: the patch
        // encoder runs in its own GraphSession per batch.
        result.submits = patch_submits + session.submits();
        result.arena_bytes = session.arena_used();
        result.seconds = seconds_since(started);
        if (progress != nullptr) progress->report(0.65f, "encoder");
        return ResultT::success(std::move(result));
    } catch (const std::exception& error) {
        return ResultT::failure(std::string("SHARP encoder failed: ") + error.what());
    }
}

}  // namespace splat
