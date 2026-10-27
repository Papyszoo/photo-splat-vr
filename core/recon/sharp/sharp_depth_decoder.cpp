#include "recon/sharp/sharp_depth_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "recon/ggml/check.h"
#include "recon/ggml/nn.h"

namespace splat {
namespace {

// dims_encoder = [last_encoder=256] + MONODEPTH_ENCODER_DIMS_MAP["dinov2l16_384"].
constexpr int kLevelChannels[5] = {256, 256, 512, 1024, 1024};
constexpr int kDecoderChannels = 256;
constexpr int kHeadHidden = 128;
constexpr int kHeadFeatures = 32;
constexpr int kLayers = 2;

double seconds_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

SharpDepthDecoder::~SharpDepthDecoder() = default;

Status SharpDepthDecoder::initialize(std::shared_ptr<SharpModel> model) {
    model_ = std::move(model);
    const GgmlWeights& weights = model_->weights();
    try {
        // convs[0] is Identity: the checkpoint has no decoder.convs.0.weight.
        if (weights.find("decoder.convs.0.weight") != nullptr) {
            return Status::error("GGUF has decoder.convs.0.weight, but the SHARP graph's level-0 "
                                 "decoder projection is an Identity");
        }
        for (int index = 1; index < 5; ++index) {
            convs_[static_cast<size_t>(index)] =
                resolve_conv(weights, "decoder.convs." + std::to_string(index));
        }
        for (int index = 0; index < 5; ++index) {
            fusions_[static_cast<size_t>(index)] =
                resolve_fusion_block(weights, "decoder.fusions." + std::to_string(index));
            // fusions[1..4] upsample x2; fusions[0] has an empty `deconv`.
            const bool upsampling = index != 0;
            const bool has_deconv = fusions_[static_cast<size_t>(index)].deconv.weight != nullptr;
            if (upsampling != has_deconv) {
                return Status::error("GGUF fusion " + std::to_string(index) +
                                     (upsampling ? " has no deconv weight" : " has an unexpected deconv"));
            }
            const int64_t channels = fusions_[static_cast<size_t>(index)].out_conv.weight->ne[2];
            if (channels != kDecoderChannels ||
                fusions_[static_cast<size_t>(index)].out_conv.weight->ne[3] != kDecoderChannels) {
                return Status::error("GGUF decoder fusion " + std::to_string(index) +
                                     " is not 256 -> 256");
            }
        }
        head_conv0_ = resolve_conv(weights, "head.0", /*bias_expected=*/true);
        head_deconv_ = resolve_conv(weights, "head.1", /*bias_expected=*/true);
        head_conv2_ = resolve_conv(weights, "head.2", /*bias_expected=*/true);
        head_last_ = resolve_conv(weights, "head.4", /*bias_expected=*/true);

        // Shape checks against the hard-coded graph.
        if (convs_[4].weight->ne[2] != kLevelChannels[4] ||
            convs_[4].weight->ne[3] != kDecoderChannels ||
            convs_[3].weight->ne[2] != kLevelChannels[3] ||
            convs_[1].weight->ne[2] != kLevelChannels[1] ||
            head_conv0_.weight->ne[2] != kDecoderChannels ||
            head_conv0_.weight->ne[3] != kHeadHidden ||
            head_conv2_.weight->ne[3] != kHeadFeatures ||
            head_last_.weight->ne[3] != kLayers ||
            head_deconv_.weight->ne[0] != 2 || head_deconv_.weight->ne[1] != 2) {
            return Status::error("GGUF does not match the SHARP depth decoder graph");
        }
    } catch (const std::exception& error) {
        return Status::error(std::string("GGUF does not match the SHARP depth decoder graph: ") +
                             error.what());
    }
    return Status::success();
}

Result<std::unique_ptr<SharpDepthDecoder>> SharpDepthDecoder::create(
    std::shared_ptr<SharpModel> model, SharpDepthConfig config) {
    using ResultT = Result<std::unique_ptr<SharpDepthDecoder>>;
    auto decoder = std::unique_ptr<SharpDepthDecoder>(new SharpDepthDecoder());
    decoder->config_ = config;
    const Status status = decoder->initialize(std::move(model));
    if (!status.ok()) return ResultT::failure(status.message());
    return ResultT::success(std::move(decoder));
}

Result<std::unique_ptr<SharpDepthDecoder>> SharpDepthDecoder::create(const Path& gguf,
                                                                     bool prefer_vulkan,
                                                                     SharpDepthConfig config) {
    using ResultT = Result<std::unique_ptr<SharpDepthDecoder>>;
    Result<std::shared_ptr<SharpModel>> model = SharpModel::load(gguf, prefer_vulkan);
    if (!model.ok()) return ResultT::failure(model.message());
    return create(std::move(model.value()), config);
}

Result<SharpDepthResult> SharpDepthDecoder::run(const std::array<ggml_tensor*, 5>& levels,
                                                int image_size, float disparity_factor,
                                                const SharpStageCallback& stage_cb,
                                                Progress* progress) {
    using ResultT = Result<SharpDepthResult>;
    const auto started = std::chrono::steady_clock::now();
    for (ggml_tensor* level : levels) {
        if (level == nullptr) return ResultT::failure("run() got a null encoder level");
    }
    if (image_size <= 0 || image_size % 2 != 0) {
        return ResultT::failure("the depth decoder needs an even network size, got " +
                                std::to_string(image_size));
    }

    try {
        SharpDepthResult result;
        result.image_size = image_size;
        result.layers = kLayers;
        GraphSession session(model_->backend(), model_->weights(), config_.nodes_per_submit,
                             config_.arena_bytes);
        ggml_context* ctx = session.ctx();

        auto stage_shape = [](const ggml_tensor* tensor) {
            return std::vector<int64_t>{tensor->ne[0], tensor->ne[1], tensor->ne[2], tensor->ne[3]};
        };
        auto emit_tensor = [&](const std::string& name, ggml_tensor* tensor) {
            if (!stage_cb) return;
            SharpStage stage;
            stage.name = name;
            stage.shape = stage_shape(tensor);
            const Status status = session.download(tensor, &stage.values);
            if (!status.ok()) throw std::runtime_error(status.message());
            stage_cb(stage);
        };

        if (progress != nullptr) progress->report(0.05f, "depth decoder");
        const auto mark = std::chrono::steady_clock::now();
        // ---- depth decoder: MultiresConvDecoder, deepest level first --------
        ggml_tensor* x = nn::conv2d(ctx, convs_[4].weight, convs_[4].bias, levels[4], 1, 1);
        x = fusion_block(ctx, fusions_[4], x, nullptr, /*upsample=*/true);
        for (int index = 3; index >= 1; --index) {
            ggml_tensor* skip =
                nn::conv2d(ctx, convs_[index].weight, convs_[index].bias,
                           levels[static_cast<size_t>(index)], 1, 1);
            x = fusion_block(ctx, fusions_[static_cast<size_t>(index)], x, skip,
                             /*upsample=*/true);
        }
        // convs[0] is the Identity, so the level-0 skip is the encoder map.
        x = fusion_block(ctx, fusions_[0], x, levels[0], /*upsample=*/false);
        require(x->ne[0] == image_size / 2 && x->ne[1] == image_size / 2 &&
                    x->ne[2] == kDecoderChannels,
                "the depth decoder did not produce 256 features at half the network size");

        // The decoder output is 604 MiB; only the parity path pays for a copy,
        // the head consumes it directly inside the same submit otherwise.
        ggml_tensor* decoder_features = x;
        if (stage_cb) {
            decoder_features = session.carry(x);
            emit_tensor("depth_decoder_out", decoder_features);
        }
        result.decoder_seconds = seconds_since(mark);

        // ---- disparity head -------------------------------------------------
        // carry() started a new segment, so take its context again.
        ctx = session.ctx();
        const auto head_started = std::chrono::steady_clock::now();
        ggml_tensor* head = nn::conv2d(ctx, head_conv0_.weight, head_conv0_.bias,
                                       decoder_features, 1, 1);
        head = nn::conv_transpose2d(ctx, head_deconv_.weight, head_deconv_.bias, head, 2);
        head = nn::conv2d(ctx, head_conv2_.weight, head_conv2_.bias, head, 1, 1);
        head = ggml_relu(ctx, head);
        head = nn::conv2d(ctx, head_last_.weight, head_last_.bias, head, 1, 0);
        head = ggml_relu(ctx, head);  // disparity
        head = session.finish(head);
        require(head->ne[0] == image_size && head->ne[1] == image_size && head->ne[2] == kLayers,
                "the disparity head did not produce 2 layers at the network size");

        const Status downloaded = session.download(head, &result.disparity);
        if (!downloaded.ok()) return ResultT::failure(downloaded.message());
        emit_tensor("depth_head_out", head);
        result.head_seconds = seconds_since(head_started);

        // ---- monodepth, then the (skipped) depth adjustment -----------------
        // models/monodepth.py:209-212 sorts the two layers only when
        // sorting_monodepth is on, and PredictorParams leaves it off, so the
        // layer order is the head's own.
        //
        // models/predictor.py:131: monodepth = disparity_factor /
        // clamp(disparity, 1e-4, 1e4), in fp32 like the reference.
        result.monodepth.resize(result.disparity.size());
        for (size_t index = 0; index < result.disparity.size(); ++index) {
            const float clamped = std::min(std::max(result.disparity[index], 1e-4f), 1e4f);
            result.monodepth[index] = disparity_factor / clamped;
        }
        // Stage 4 is not implemented on purpose: the reference's forward pass
        // passes depth=None, so its DepthAlignment returns an all-ones map and
        // the monodepth above is already the aligned output. The parity test
        // checks that against the reference's own 040_depth_align_* dumps.

        result.submits = session.submits();
        result.arena_bytes = session.arena_used();
        result.seconds = seconds_since(started);
        if (progress != nullptr) progress->report(1.0f, "depth decoder");
        return ResultT::success(std::move(result));
    } catch (const std::exception& error) {
        return ResultT::failure(std::string("SHARP depth decoder failed: ") + error.what());
    }
}

}  // namespace splat
