#include "recon/sharp/sharp_reconstructor.h"

#include <chrono>
#include <utility>

#include "common/exif.h"
#include "recon/sharp/sharp_composer.h"
#include "recon/sharp/sharp_initializer.h"
#include "recon/sharp/sharp_preprocess.h"

namespace splat {
namespace {

double seconds_since(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

SharpReconstructor::SharpReconstructor(SharpConfig config) : config_(config) {}

SharpReconstructor::~SharpReconstructor() { unload(); }

BackendInfo SharpReconstructor::info() const {
    BackendInfo backend;
    backend.name = "sharp";
    backend.license =
        "Apple ml-sharp LICENSE_MODEL (research use only; weights fetched and converted locally)";
    // 655.5 M live parameters = 1.22 GiB fp16, plus the encoder's 1.06 GiB level
    // arena and the decoder graphs; the spec budget is 8 GiB.
    backend.vram_bytes = 5ull << 30;
    return backend;
}

Status SharpReconstructor::load(const Path& gguf) {
    unload();
    Result<std::shared_ptr<SharpModel>> model = SharpModel::load(gguf, config_.prefer_vulkan);
    if (!model.ok()) return Status::error(model.message());
    model_ = std::move(model.value());

    Result<std::unique_ptr<SharpEncoder>> encoder =
        SharpEncoder::create(model_, config_.encoder);
    if (!encoder.ok()) return Status::error(encoder.message());
    encoder_ = encoder.take();

    Result<std::unique_ptr<SharpDepthDecoder>> depth =
        SharpDepthDecoder::create(model_, config_.depth);
    if (!depth.ok()) return Status::error(depth.message());
    depth_ = depth.take();

    Result<std::unique_ptr<SharpGaussianDecoder>> gaussians =
        SharpGaussianDecoder::create(model_, config_.gaussians);
    if (!gaussians.ok()) return Status::error(gaussians.message());
    gaussians_ = gaussians.take();
    return Status::success();
}

void SharpReconstructor::unload() {
    gaussians_.reset();
    depth_.reset();
    encoder_.reset();
    model_.reset();
    last_monodepth_.clear();
    timings_ = SharpTimings{};
}

Result<SplatScene> SharpReconstructor::run(const ReconstructionInput& input, Progress& progress) {
    using ResultT = Result<SplatScene>;
    if (encoder_ == nullptr || depth_ == nullptr || gaussians_ == nullptr) {
        return ResultT::failure("SHARP backend is not loaded");
    }
    const Image& image = input.rgb;
    if (image.empty()) return ResultT::failure("SHARP backend got an empty image");
    const int width = image.width;
    const int height = image.height;

    const auto started = std::chrono::steady_clock::now();
    timings_ = SharpTimings{};

    // ---- stage 0: preprocess ------------------------------------------------
    auto mark = std::chrono::steady_clock::now();
    progress.report(0.02f, "preprocess");
    Result<SharpImage> preprocessed =
        sharp_preprocess(image.rgb.data(), width, height, config_.encoder.image_size);
    if (!preprocessed.ok()) return ResultT::failure(preprocessed.message());
    std::vector<float> input_chw = std::move(preprocessed.value().chw);
    const int image_size = preprocessed.value().size;
    // F.interpolate(align_corners=True) of the photo, kept in [0,1] for the
    // initialiser before the encoder's own 2x-1 normalisation.
    const std::vector<float> image01 = input_chw;
    sharp_normalise(&input_chw);
    timings_.preprocess = seconds_since(mark);

    // The focal length comes from EXIF via the job runner; SHARP's own default
    // (30 mm, 35 mm equivalent) is what the runner falls back to, so anything
    // it hands over is used as-is. Only the ratio f_px/width conditions the
    // network; the absolute value sets the metric scale.
    float focal_px = input.focal_px.value_or(0.0f);
    if (focal_px <= 0.0f) {
        const Result<float> fallback = focal_px_from_exif(ExifFocal{}, width, height);
        if (!fallback.ok()) return ResultT::failure(fallback.message());
        focal_px = fallback.value();
    }
    last_focal_px_ = focal_px;
    const float disparity_factor = focal_px / static_cast<float>(width);

    // ---- stage 2: encoder ---------------------------------------------------
    mark = std::chrono::steady_clock::now();
    Progress encoder_progress([&progress](float fraction, const std::string& stage) {
        progress.report(0.03f + 0.62f * fraction, stage.empty() ? "encoder" : stage);
    });
    Result<SharpEncoderResult> encoded =
        encoder_->run(input_chw.data(), image_size, {}, &encoder_progress);
    if (!encoded.ok()) return ResultT::failure(encoded.message());
    timings_.encoder = seconds_since(mark);
    std::vector<float>().swap(input_chw);

    std::array<ggml_tensor*, 5> levels{{encoder_->level(0), encoder_->level(1), encoder_->level(2),
                                       encoder_->level(3), encoder_->level(4)}};

    // ---- stages 3 + 4: depth decoder, adjustment skipped --------------------
    mark = std::chrono::steady_clock::now();
    Progress depth_progress([&progress](float fraction, const std::string& stage) {
        progress.report(0.65f + 0.10f * fraction, stage.empty() ? "depth decoder" : stage);
    });
    Result<SharpDepthResult> depth =
        depth_->run(levels, image_size, disparity_factor, {}, &depth_progress);
    if (!depth.ok()) return ResultT::failure(depth.message());
    timings_.depth_decoder = seconds_since(mark);
    last_monodepth_ = depth.value().monodepth;

    // ---- stage 6a: initialiser ---------------------------------------------
    mark = std::chrono::steady_clock::now();
    progress.report(0.76f, "initialiser");
    Result<SharpBaseValues> base =
        sharp_initialize(image01.data(), image_size, last_monodepth_.data(), depth.value().layers);
    if (!base.ok()) return ResultT::failure(base.message());
    timings_.initializer = seconds_since(mark);

    // ---- stage 5: Gaussian decoder ------------------------------------------
    mark = std::chrono::steady_clock::now();
    Progress gaussian_progress([&progress](float fraction, const std::string& stage) {
        progress.report(0.77f + 0.15f * fraction, stage.empty() ? "Gaussian decoder" : stage);
    });
    Result<SharpGaussianResult> delta =
        gaussians_->run(levels, base.value().feature_input.data(), image_size, {},
                        &gaussian_progress);
    if (!delta.ok()) return ResultT::failure(delta.message());
    timings_.gaussian_decoder = seconds_since(mark);

    // ---- stage 6b: composer + unprojection ---------------------------------
    mark = std::chrono::steady_clock::now();
    progress.report(0.93f, "composer");
    Result<SharpComposedGaussians> composed = sharp_compose(base.value(), delta.value().delta.data());
    if (!composed.ok()) return ResultT::failure(composed.message());
    Result<SplatScene> scene = sharp_unproject(composed.value(), focal_px, width, height);
    if (!scene.ok()) return ResultT::failure(scene.message());
    timings_.composer = seconds_since(mark);

    timings_.total = seconds_since(started);
    progress.report(1.0f, "done");
    return ResultT::success(std::move(scene.value()));
}

}  // namespace splat
