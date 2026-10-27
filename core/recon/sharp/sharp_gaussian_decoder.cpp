#include "recon/sharp/sharp_gaussian_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "recon/ggml/check.h"
#include "recon/ggml/nn.h"

namespace splat {
namespace {

// dims_decoder = (128, 128, 128, 128, 128) over the monodepth encoder levels.
constexpr int kLevelChannels[5] = {256, 256, 512, 1024, 1024};
constexpr int kDecoderChannels = 128;
constexpr int kHeadChannels = 64;
constexpr int kHeadFeatures = 32;
constexpr int kHeadGroups = 8;
constexpr int kHeadLayers = 2;
constexpr float kGroupNormEps = 1e-5f;
constexpr int kDeltaAttributes = 14;
constexpr int kGeometryAttributes = 3;  // mean (3), scale (3) share the geometry head

double seconds_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

SharpGroupNormResidual resolve_residual(const GgmlWeights& weights, const std::string& prefix) {
    SharpGroupNormResidual block;
    block.norm1 = resolve_conv(weights, prefix + ".residual.0", /*bias_expected=*/true);
    block.conv1 = resolve_conv(weights, prefix + ".residual.2", /*bias_expected=*/true);
    block.norm2 = resolve_conv(weights, prefix + ".residual.3", /*bias_expected=*/true);
    block.conv2 = resolve_conv(weights, prefix + ".residual.5", /*bias_expected=*/true);
    return block;
}

SharpHeadWeights resolve_head(const GgmlWeights& weights, const std::string& prefix) {
    SharpHeadWeights head;
    head.block[0] = resolve_residual(weights, prefix + ".0");
    head.block[1] = resolve_residual(weights, prefix + ".1");
    head.out = resolve_conv(weights, prefix + ".3", /*bias_expected=*/true);
    return head;
}

// residual_block_2d: x + conv2(relu(gn2(conv1(relu(gn1(x)))))).
ggml_tensor* group_residual(ggml_context* ctx, const SharpGroupNormResidual& weights,
                            ggml_tensor* x) {
    ggml_tensor* out =
        nn::group_norm(ctx, x, weights.norm1.weight, weights.norm1.bias, kHeadGroups, kGroupNormEps);
    out = ggml_relu(ctx, out);
    out = nn::conv2d(ctx, weights.conv1.weight, weights.conv1.bias, out, 1, 1);
    out = nn::group_norm(ctx, out, weights.norm2.weight, weights.norm2.bias, kHeadGroups,
                         kGroupNormEps);
    out = ggml_relu(ctx, out);
    out = nn::conv2d(ctx, weights.conv2.weight, weights.conv2.bias, out, 1, 1);
    return ggml_add(ctx, out, x);
}

ggml_tensor* head_forward(ggml_context* ctx, const SharpHeadWeights& weights, ggml_tensor* x) {
    x = group_residual(ctx, weights.block[0], x);
    x = group_residual(ctx, weights.block[1], x);
    x = ggml_relu(ctx, x);
    x = nn::conv2d(ctx, weights.out.weight, weights.out.bias, x, 1, 0);
    return ggml_relu(ctx, x);
}

}  // namespace

SharpGaussianDecoder::~SharpGaussianDecoder() = default;

Status SharpGaussianDecoder::initialize(std::shared_ptr<SharpModel> model) {
    model_ = std::move(model);
    const GgmlWeights& weights = model_->weights();
    try {
        image_encoder_ = resolve_conv(weights, "feature_model.image_encoder.conv",
                                      /*bias_expected=*/true);
        for (int index = 0; index < 5; ++index) {
            convs_[static_cast<size_t>(index)] =
                resolve_conv(weights, "feature_model.decoder.convs." + std::to_string(index),
                             /*bias_expected=*/false);
            fusions_[static_cast<size_t>(index)] =
                resolve_fusion_block(weights, "feature_model.decoder.fusions." + std::to_string(index));
            const bool upsampling = index != 0;
            const bool has_deconv = fusions_[static_cast<size_t>(index)].deconv.weight != nullptr;
            if (upsampling != has_deconv) {
                return Status::error("GGUF Gaussian-decoder fusion " + std::to_string(index) +
                                     (upsampling ? " has no deconv weight" : " has an unexpected deconv"));
            }
        }
        fusion_ = resolve_fusion_block(weights, "feature_model.fusion");
        if (fusion_.deconv.weight != nullptr) {
            return Status::error("GGUF feature_model.fusion has a deconv, but the Gaussian decoder "
                                 "fuses at the output stride (nn.Identity)");
        }
        texture_head_ = resolve_head(weights, "feature_model.texture_head");
        geometry_head_ = resolve_head(weights, "feature_model.geometry_head");
        prediction_geometry_ = resolve_conv(weights, "prediction_head.geometry_prediction_head",
                                            /*bias_expected=*/true);
        prediction_texture_ = resolve_conv(weights, "prediction_head.texture_prediction_head",
                                           /*bias_expected=*/true);

        // Shape checks against the hard-coded graph.
        const int64_t geometry_out = prediction_geometry_.weight->ne[3];
        const int64_t texture_out = prediction_texture_.weight->ne[3];
        if (image_encoder_.weight->ne[2] != 5 || image_encoder_.weight->ne[3] != kDecoderChannels ||
            image_encoder_.weight->ne[0] != 2 ||
            convs_[0].weight->ne[0] != 1 || convs_[0].weight->ne[2] != kLevelChannels[0] ||
            convs_[4].weight->ne[2] != kLevelChannels[4] ||
            convs_[0].weight->ne[3] != kDecoderChannels ||
            fusions_[0].out_conv.weight->ne[2] != kDecoderChannels ||
            fusion_.out_conv.weight->ne[2] != kDecoderChannels ||
            texture_head_.out.weight->ne[3] != kHeadFeatures ||
            geometry_head_.out.weight->ne[3] != kHeadFeatures ||
            geometry_out != kGeometryAttributes * kHeadLayers ||
            texture_out != (kDeltaAttributes - kGeometryAttributes) * kHeadLayers) {
            return Status::error("GGUF does not match the SHARP Gaussian decoder graph");
        }
        if (texture_head_.block[0].norm1.weight->ne[0] != kDecoderChannels ||
            texture_head_.block[0].conv1.weight->ne[3] != kHeadChannels ||
            texture_head_.block[0].conv2.weight->ne[2] != kHeadChannels) {
            return Status::error("GGUF does not match the SHARP Gaussian head graph");
        }
    } catch (const std::exception& error) {
        return Status::error(std::string("GGUF does not match the SHARP Gaussian decoder graph: ") +
                             error.what());
    }
    return Status::success();
}

Result<std::unique_ptr<SharpGaussianDecoder>> SharpGaussianDecoder::create(
    std::shared_ptr<SharpModel> model, SharpGaussianConfig config) {
    using ResultT = Result<std::unique_ptr<SharpGaussianDecoder>>;
    auto decoder = std::unique_ptr<SharpGaussianDecoder>(new SharpGaussianDecoder());
    decoder->config_ = config;
    const Status status = decoder->initialize(std::move(model));
    if (!status.ok()) return ResultT::failure(status.message());
    return ResultT::success(std::move(decoder));
}

Result<std::unique_ptr<SharpGaussianDecoder>> SharpGaussianDecoder::create(const Path& gguf,
                                                                           bool prefer_vulkan,
                                                                           SharpGaussianConfig config) {
    using ResultT = Result<std::unique_ptr<SharpGaussianDecoder>>;
    Result<std::shared_ptr<SharpModel>> model = SharpModel::load(gguf, prefer_vulkan);
    if (!model.ok()) return ResultT::failure(model.message());
    return create(std::move(model.value()), config);
}

Result<SharpGaussianResult> SharpGaussianDecoder::run(const std::array<ggml_tensor*, 5>& levels,
                                                      const float* feature_input, int image_size,
                                                      const SharpStageCallback& stage_cb,
                                                      Progress* progress) {
    using ResultT = Result<SharpGaussianResult>;
    const auto started = std::chrono::steady_clock::now();
    for (ggml_tensor* level : levels) {
        if (level == nullptr) return ResultT::failure("run() got a null encoder level");
    }
    if (feature_input == nullptr) return ResultT::failure("run() got a null feature input");
    if (image_size <= 0 || image_size % 2 != 0) {
        return ResultT::failure("the Gaussian decoder needs an even network size, got " +
                                std::to_string(image_size));
    }
    const int grid = image_size / 2;

    try {
        SharpGaussianResult result;
        result.grid = grid;
        result.layers = kHeadLayers;
        const size_t pixels = static_cast<size_t>(grid) * grid;
        GraphSession session(model_->backend(), model_->weights(), config_.nodes_per_submit,
                             config_.arena_bytes);
        ggml_context* ctx = session.ctx();

        if (progress != nullptr) progress->report(0.05f, "Gaussian decoder");
        // The 5-channel initialiser output is the only network input that has to
        // be uploaded; the encoder levels already live in the model's backend.
        ggml_tensor* features_in = session.persistent("gaussian_feature_input",
                                                      {image_size, image_size, 5, 1});
        Status uploaded = model_->weights().upload(
            features_in, feature_input, static_cast<size_t>(5) * image_size * image_size * sizeof(float));
        if (!uploaded.ok()) return ResultT::failure(uploaded.message());

        // models/gaussian_decoder.py:240-262.
        ggml_tensor* skip = nn::conv2d(ctx, image_encoder_.weight, image_encoder_.bias,
                                       features_in, 2, 0);
        require(skip->ne[0] == grid && skip->ne[1] == grid &&
                    skip->ne[2] == kDecoderChannels,
                "the image-encoder skip conv did not produce 128 features at half the size");

        ggml_tensor* x = nn::conv2d(ctx, convs_[4].weight, convs_[4].bias, levels[4], 1, 1);
        x = fusion_block(ctx, fusions_[4], x, nullptr, /*upsample=*/true);
        for (int index = 3; index >= 1; --index) {
            ggml_tensor* level_skip =
                nn::conv2d(ctx, convs_[index].weight, convs_[index].bias,
                           levels[static_cast<size_t>(index)], 1, 1);
            x = fusion_block(ctx, fusions_[static_cast<size_t>(index)], x, level_skip,
                             /*upsample=*/true);
        }
        ggml_tensor* level0 = nn::conv2d(ctx, convs_[0].weight, convs_[0].bias, levels[0], 1, 0);
        x = fusion_block(ctx, fusions_[0], x, level0, /*upsample=*/false);
        // stride_out == 2, so `self.upsample` is nn.Identity.
        x = fusion_block(ctx, fusion_, x, skip, /*upsample=*/false);
        require(x->ne[0] == grid && x->ne[1] == grid && x->ne[2] == kDecoderChannels,
                "the Gaussian decoder did not produce 128 features at the output stride");

        // The two 32-channel head outputs are stage boundaries (060_* dumps),
        // so they are carried into the arena; the prediction heads then run on
        // them and their 22+6 channel outputs are concatenated into one submit.
        ggml_tensor* texture = head_forward(ctx, texture_head_, x);
        texture = session.carry(texture);
        ggml_tensor* geometry = head_forward(ctx, geometry_head_, x);
        geometry = session.finish(geometry);

        if (stage_cb) {
            SharpStage texture_stage;
            texture_stage.name = "gaussian_decoder_out_texture";
            texture_stage.shape = {grid, grid, kHeadFeatures, 1};
            const Status status = session.download(texture, &texture_stage.values);
            if (!status.ok()) return ResultT::failure(status.message());
            stage_cb(texture_stage);

            SharpStage geometry_stage;
            geometry_stage.name = "gaussian_decoder_out_geometry";
            geometry_stage.shape = {grid, grid, kHeadFeatures, 1};
            const Status geometry_status = session.download(geometry, &geometry_stage.values);
            if (!geometry_status.ok()) return ResultT::failure(geometry_status.message());
            stage_cb(geometry_stage);
        }

        // models/heads.py:30-36: 1x1 to (3, layers) and (11, layers).
        // carry()/finish() above started new segments, so take the current one.
        ctx = session.ctx();
        ggml_tensor* texture_delta = nn::conv2d(ctx, prediction_texture_.weight,
                                               prediction_texture_.bias, texture, 1, 0);
        ggml_tensor* geometry_delta = nn::conv2d(ctx, prediction_geometry_.weight,
                                                prediction_geometry_.bias, geometry, 1, 0);
        const int texture_channels = (kDeltaAttributes - kGeometryAttributes) * kHeadLayers;
        const int geometry_channels = kGeometryAttributes * kHeadLayers;
        require(texture_delta->ne[2] == texture_channels &&
                    geometry_delta->ne[2] == geometry_channels,
                "the prediction heads did not produce 14 attributes per layer");
        ggml_tensor* both = ggml_concat(ctx, texture_delta, geometry_delta, 2);
        both = session.finish(both);
        std::vector<float> values;
        const Status downloaded = session.download(both, &values);
        if (!downloaded.ok()) return ResultT::failure(downloaded.message());
        const size_t delta_pixels =
            static_cast<size_t>(texture_channels + geometry_channels) * pixels;
        require(values.size() == delta_pixels, "unexpected prediction-head output size");

        const float* texture_values = values.data();
        const float* geometry_values = values.data() + static_cast<size_t>(texture_channels) * pixels;

        // models/heads.py:48-52 interleaves (attribute, layer) into the head
        // channels; the composer reads (14, layers, grid, grid).
        result.delta.assign(static_cast<size_t>(kDeltaAttributes) * kHeadLayers * pixels, 0.0f);
        for (int attribute = 0; attribute < kDeltaAttributes; ++attribute) {
            const bool is_geometry = attribute < kGeometryAttributes;
            const float* source = is_geometry ? geometry_values : texture_values;
            const int source_attribute =
                is_geometry ? attribute : attribute - kGeometryAttributes;
            for (int layer = 0; layer < kHeadLayers; ++layer) {
                const size_t channel = static_cast<size_t>(source_attribute * kHeadLayers + layer);
                const float* plane = source + channel * pixels;
                float* destination = result.delta.data() +
                                     (static_cast<size_t>(attribute) * kHeadLayers + layer) * pixels;
                std::memcpy(destination, plane, pixels * sizeof(float));
            }
        }

        result.submits = session.submits();
        result.arena_bytes = session.arena_used();
        result.seconds = seconds_since(started);
        if (progress != nullptr) progress->report(1.0f, "Gaussian decoder");
        return ResultT::success(std::move(result));
    } catch (const std::exception& error) {
        return ResultT::failure(std::string("SHARP Gaussian decoder failed: ") + error.what());
    }
}

}  // namespace splat
