// Task AA: edges blink when the head moves.
//
// The VR2 user report on PSVR2 (1984x2048 per eye): "when I move my head some
// edges blink; a bit of anti-aliasing could make it better". This test renders
// a short headset-eye sequence of the immichvr-1 SHARP scene through the real
// renderer and measures the temporal flicker, so the anti-aliasing fix can be
// checked on the desktop instead of in the headset.
//
// Metric: the temporal second difference (jerk) of the frame sequence,
//
//   J = mean |I(k+1) - 2 I(k) + I(k-1)|
//
// over a crop inside the capture photo region. The sequence moves the head at a
// constant, sub-pixel velocity, so a band-limited render changes almost linearly
// in time and J is near zero; aliasing shows up as splats popping in and out
// between frames, which is exactly what the user sees. J is reported over all
// crop pixels and over the strongest 10 % of edges (the blinking edges), plus
// the share of pixels that jump by more than 8 levels between consecutive
// frames. A still-frame PSNR against the current (eps2d 0) render and the pass
// timings guard against buying flicker with blur or time.
//
// The scene is external data, so the GPU case skips when it is absent. PNG
// crops and the flicker maps go to $SPLAT_DATA_DIR/runs/aa/.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <stb/stb_image_write.h>

#include "common/image.h"
#include "data_dir.h"
#include "jobs/image_io.h"
#include "render/hmd_view.h"
#include "render/project_math.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/scene_folder.h"

#if defined(SPLAT_HAVE_VULKAN)
#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif
#endif

namespace {

// PSVR2's eye image and the tangents Hmd2 reported (see vr_view_test.cpp).
constexpr uint32_t kEyeWidth = 1984;
constexpr uint32_t kEyeHeight = 2048;
constexpr splat::HeadsetFov kPsvr2Fov{1.842f, 0.947f, 1.329f, 1.329f};

// The crop inside the capture photo. At the capture pose the photo covers
// x 893..1729 (left eye) and 249..1088 (right) of the PSVR2 eye, y 685..1365:
// the 1984x2048 eye is much wider than the capture, so most of it is backdrop
// and the two eyes sit the photo at different x (the +-16 % principal-point
// shift). These rectangles stay inside the photo, so the comfort and
// capture-frustum fades are out of the measurement.
constexpr int kCropX[2] = {940, 300};
constexpr int kCropY = 720;
constexpr int kCropWidth = 600;
constexpr int kCropHeight = 590;

// Head motion, per frame: 0.05 deg yaw (0.62 px at the eye's fx), 0.02 deg
// pitch and 1 mm sideways, all sub-pixel at the scene's depth range. Seven
// frames is six second differences and 3.7 px of total travel.
constexpr int kFrameCount = 7;
constexpr double kYawStep = 0.05 * 3.14159265358979323846 / 180.0;
constexpr double kPitchStep = 0.02 * 3.14159265358979323846 / 180.0;
constexpr float kMoveStep = 0.001f;

std::filesystem::path scene_folder() {
    return splat_test::data_path("runs/sharp_e2e/immichvr-1");
}

std::filesystem::path output_folder() { return splat_test::data_path("runs/aa"); }

splat::FrameView pose_at(int frame, float motion) {
    splat::FrameView view;
    const float step = static_cast<float>(frame) * motion;
    view.midpoint.position[0] = kMoveStep * step;
    view.midpoint.position[1] = -0.4f * kMoveStep * step;
    view.midpoint.yaw = static_cast<float>(kYawStep) * step;
    view.midpoint.pitch = static_cast<float>(kPitchStep) * step;
    view.ipd = 0.066f;
    view.comfort_fade = 1.0f;
    return view;
}

// One eye's crop, as a luminance plane. Both eyes are measured; the crop
// coordinates are per eye.
std::vector<float> eye_crop_luma(const splat::Image& side_by_side, int eye) {
    std::vector<float> out(static_cast<size_t>(kCropWidth) * kCropHeight);
    const int x0 = eye * static_cast<int>(kEyeWidth) + kCropX[eye];
    for (int y = 0; y < kCropHeight; ++y) {
        const uint8_t* row = side_by_side.pixel(x0, kCropY + y);
        for (int x = 0; x < kCropWidth; ++x) {
            out[static_cast<size_t>(y) * kCropWidth + x] =
                (row[x * 3 + 0] + row[x * 3 + 1] + row[x * 3 + 2]) / 3.0f;
        }
    }
    return out;
}

splat::Image crop_image(const splat::Image& side_by_side, int eye) {
    splat::Image out;
    out.resize(kCropWidth, kCropHeight);
    const int x0 = eye * static_cast<int>(kEyeWidth) + kCropX[eye];
    for (int y = 0; y < kCropHeight; ++y) {
        std::copy(side_by_side.pixel(x0, kCropY + y),
                  side_by_side.pixel(x0, kCropY + y) + kCropWidth * 3,
                  out.pixel(0, y));
    }
    return out;
}

// Amplified |second difference| map of the sequence: bright where an edge
// flickers. 4x gain, clamped.
splat::Image flicker_map(const std::vector<std::vector<float>>& sequence) {
    splat::Image out;
    out.resize(kCropWidth, kCropHeight);
    for (int y = 0; y < kCropHeight; ++y) {
        for (int x = 0; x < kCropWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kCropWidth + x;
            float peak = 0.0f;
            for (size_t k = 1; k + 1 < sequence.size(); ++k) {
                peak = std::max(peak, std::abs(sequence[k + 1][i] - 2.0f * sequence[k][i] +
                                               sequence[k - 1][i]));
            }
            const int value = static_cast<int>(std::min(255.0f, peak * 4.0f));
            uint8_t* p = out.pixel(x, y);
            p[0] = p[1] = p[2] = static_cast<uint8_t>(value);
        }
    }
    return out;
}

double psnr(const splat::Image& a, const splat::Image& b) {
    if (a.width != b.width || a.height != b.height) return -1.0;
    double squared = 0.0;
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const double difference = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
        squared += difference * difference;
    }
    if (squared <= 0.0) return 1.0e9;
    return 10.0 * std::log10((255.0 * 255.0) / (squared / (static_cast<double>(a.rgb.size()))));
}

bool write_png(const std::filesystem::path& path, const splat::Image& image) {
    if (image.empty()) return false;
    return stbi_write_png(path.string().c_str(), image.width, image.height, 3,
                          image.rgb.data(), image.width * 3) != 0;
}

struct Measurements {
    double jerk = 0.0;        // mean |second difference|, 0-255 levels
    double jerk_edge = 0.0;   // the same over the strongest 10 % of edges
    double pops = 0.0;        // % of pixels that jump > 8 levels per step
    double gradient = 0.0;    // mean |dI/dx| + |dI/dy|, luminance levels
    double psnr_vs_base = 0.0;
    double psnr_vs_fixed = 0.0;  // against the eps2d 0.3 + compensation frame
    double project_ms = 0.0;
    double sort_ms = 0.0;
    double draw_ms = 0.0;
    splat::Image first_frame;
};

#if defined(SPLAT_HAVE_VULKAN)

// One variant of the renderer: the options under test.
struct Variant {
    const char* name = "";
    float low_pass_eps = 0.0f;
    bool compensate = true;
    float near_fade_end = 0.08f;
    bool unorm_target = false;
    float motion = 1.0f;  // 0 repeats one pose: the metric's zero control
};

splat::RendererOptions make_options(const Variant& variant) {
    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = true;
    options.target_format =
        variant.unorm_target ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
    const splat::HeadsetProjection projection =
        splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    options.intrinsics = projection.intrinsics;
    options.eye_center_shift[0] = projection.eye_shift[0];
    options.eye_center_shift[1] = projection.eye_shift[1];
    options.low_pass_eps = variant.low_pass_eps;
    options.low_pass_compensate = variant.compensate;
    options.near_fade_end = variant.near_fade_end;
    return options;
}

// Renders the sequence for one variant and fills `out`. Recreates the renderer
// (and the target image when the colour format changes) for every variant.
bool render_sequence(splat::VulkanContext& context, const splat::SplatScene& scene,
                     const Variant& variant, const std::filesystem::path& out_folder,
                     Measurements& out, std::string& error) {
    const splat::RendererOptions options = make_options(variant);
    auto renderer_result = splat::SplatRenderer::create(context, options, scene);
    if (!renderer_result.ok()) {
        error = renderer_result.message();
        return false;
    }
    std::unique_ptr<splat::SplatRenderer> renderer = renderer_result.take();

    auto image_result = splat::create_vulkan_color_image(
        context, options.target_format, renderer->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image_result.ok()) {
        error = image_result.message();
        return false;
    }
    splat::VulkanImage image = image_result.take();
    splat::RenderTarget target;
    target.image = image.image;
    target.view = image.view;
    target.extent = renderer->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // Per eye: the luminance sequence over the sequence's frames, and the jerk
    // and pop sums.
    std::vector<std::vector<std::vector<float>>> luma(2);
    for (auto& eye : luma) eye.reserve(kFrameCount);
    double jerk_sum = 0.0;
    double jerk_edge_sum = 0.0;
    double pops_sum = 0.0;
    double gradient_sum = 0.0;
    double project_ms = 0.0, sort_ms = 0.0, draw_ms = 0.0;
    size_t jerk_samples = 0;
    size_t edge_samples = 0;

    for (int frame = 0; frame < kFrameCount; ++frame) {
        const splat::Status status = renderer->render(pose_at(frame, variant.motion), target, true);
        if (!status.ok()) {
            error = status.message();
            splat::destroy_vulkan_image(context, image);
            return false;
        }
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer->read_image();
        if (!pixels.ok()) {
            error = pixels.message();
            splat::destroy_vulkan_image(context, image);
            return false;
        }
        for (int eye = 0; eye < 2; ++eye) luma[eye].push_back(eye_crop_luma(pixels.value(), eye));
        if (frame == 0) {
            out.first_frame = std::move(pixels.value());
        } else {
            // Warm-up frame excluded from the timings (idle GPU clock).
            const splat::PassTimings& timings = renderer->timings();
            project_ms += timings.project_ms;
            sort_ms += timings.sort_ms;
            draw_ms += timings.draw_ms;
        }
    }
    splat::destroy_vulkan_image(context, image);

    const double timing_frames = static_cast<double>(kFrameCount - 1);
    out.project_ms = project_ms / timing_frames;
    out.sort_ms = sort_ms / timing_frames;
    out.draw_ms = draw_ms / timing_frames;

    for (int eye = 0; eye < 2; ++eye) {
        const std::vector<std::vector<float>>& frames = luma[eye];
        // Edge threshold from the middle frame's gradient magnitude: the top
        // 10 % of the crop, where the blinking edges are.
        std::vector<float> gradient(static_cast<size_t>(kCropWidth) * kCropHeight, 0.0f);
        for (int y = 1; y + 1 < kCropHeight; ++y) {
            for (int x = 1; x + 1 < kCropWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kCropWidth + x;
                const float dx = 0.5f * (frames[kFrameCount / 2][i + 1] -
                                         frames[kFrameCount / 2][i - 1]);
                const float dy = 0.5f * (frames[kFrameCount / 2][i + kCropWidth] -
                                         frames[kFrameCount / 2][i - kCropWidth]);
                gradient[i] = std::abs(dx) + std::abs(dy);
                gradient_sum += gradient[i];
            }
        }
        std::vector<float> sorted = gradient;
        std::sort(sorted.begin(), sorted.end());
        const float edge_threshold = sorted[sorted.size() * 9 / 10];

        for (int y = 1; y + 1 < kCropHeight; ++y) {
            for (int x = 1; x + 1 < kCropWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * kCropWidth + x;
                for (int k = 1; k + 1 < kFrameCount; ++k) {
                    const float second = std::abs(frames[k + 1][i] - 2.0f * frames[k][i] +
                                                  frames[k - 1][i]);
                    jerk_sum += second;
                    ++jerk_samples;
                    if (second > 8.0f) pops_sum += 1.0;
                    if (gradient[i] >= edge_threshold) {
                        jerk_edge_sum += second;
                        ++edge_samples;
                    }
                }
            }
        }
    }
    out.jerk = jerk_sum / static_cast<double>(jerk_samples);
    out.jerk_edge = edge_samples > 0 ? jerk_edge_sum / static_cast<double>(edge_samples) : 0.0;
    out.pops = 100.0 * pops_sum / static_cast<double>(jerk_samples);
    out.gradient = gradient_sum / (2.0 * static_cast<double>(kCropWidth - 2) *
                                   static_cast<double>(kCropHeight - 2));

    // Report artefacts: the first frame's crop and the flicker map (left eye).
    std::string stem;
    for (const char* c = variant.name; *c != '\0'; ++c) {
        stem += (*c == ' ' || *c == '(' || *c == ')' || *c == '.' || *c == ',' || *c == '/')
                    ? '_'
                    : *c;
    }
    write_png(out_folder / (stem + "_crop.png"), crop_image(out.first_frame, 0));
    write_png(out_folder / (stem + "_flicker.png"), flicker_map(luma[0]));
    return true;
}

// One offscreen frame at the given options; used by the still-frame check.
bool render_still(splat::VulkanContext& context, const splat::SplatScene& scene,
                  const splat::RendererOptions& options, splat::Image& out,
                  std::string& error) {
    auto renderer_result = splat::SplatRenderer::create(context, options, scene);
    if (!renderer_result.ok()) {
        error = renderer_result.message();
        return false;
    }
    std::unique_ptr<splat::SplatRenderer> renderer = renderer_result.take();
    auto image_result = splat::create_vulkan_color_image(
        context, options.target_format, renderer->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image_result.ok()) {
        error = image_result.message();
        return false;
    }
    splat::VulkanImage image = image_result.take();
    splat::RenderTarget target;
    target.image = image.image;
    target.view = image.view;
    target.extent = renderer->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const splat::Status status = renderer->render(splat::FrameView{}, target, true);
    if (!status.ok()) {
        splat::destroy_vulkan_image(context, image);
        error = status.message();
        return false;
    }
    auto pixels = renderer->read_image();
    splat::destroy_vulkan_image(context, image);
    if (!pixels.ok()) {
        error = pixels.message();
        return false;
    }
    out = std::move(pixels.value());
    return true;
}

#endif  // SPLAT_HAVE_VULKAN

}  // namespace

// Why a low-pass is needed at all: the PSVR2 eye's focal length (711 px) is
// 0.43x the capture's (1664 px), so the same splats project 2.3x smaller in the
// headset than in SHARP's own still. A large share of them ends up below one
// pixel, where point sampling flickers as the head moves.
TEST_CASE("headset eye shrinks splats below a pixel (CPU)") {
    const std::filesystem::path folder = scene_folder();
    if (!std::filesystem::exists(folder / "scene.psplat")) {
        MESSAGE("skipping: no scene at " << (folder / "scene.psplat"));
        return;
    }
    auto loaded = splat::read_scene_path(splat::Path((folder / "scene.psplat").string()));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    const splat::SplatScene& scene = loaded.value().scene;
    REQUIRE(scene.count() > 100000);

    const splat::HeadsetProjection projection =
        splat::hmd_projection(kPsvr2Fov, kEyeWidth, kEyeHeight);
    const float capture_fx = loaded.value().meta.intrinsics.fx;
    REQUIRE(capture_fx > 0.0f);

    struct Counts {
        size_t visible = 0;
        size_t below_one = 0;
        size_t below_half = 0;
        size_t near = 0;     // inside the near-fade band, 0.05..0.12 m
        size_t capped = 0;   // 3-sigma reach above the 512 px cap
        float max_reach = 0.0f;
        std::vector<float> sigma;
    };
    auto count = [&scene](float fx, float fy, float cx, float cy, uint32_t width,
                          uint32_t height) {
        Counts counts;
        counts.sigma.reserve(scene.count());
        for (size_t i = 0; i < scene.count(); ++i) {
            const float x = scene.position[i * 3 + 0];
            const float y = scene.position[i * 3 + 1];
            const float z = scene.position[i * 3 + 2];
            if (!(z > 0.05f)) continue;
            const float px = fx * x / z + cx;
            const float py = fy * y / z + cy;
            if (px < 0.0f || px > static_cast<float>(width) || py < 0.0f ||
                py > static_cast<float>(height)) {
                continue;
            }
            if (!(1.0f / (1.0f + std::exp(-scene.opacity_logit[i])) > 1.0f / 255.0f)) continue;
            if (z < 0.12f) ++counts.near;
            const splat::Mat3 rotation = splat::quaternion_to_rotation(
                scene.rotation[i * 4 + 0], scene.rotation[i * 4 + 1],
                scene.rotation[i * 4 + 2], scene.rotation[i * 4 + 3]);
            const splat::Mat3 cov3d = splat::gaussian_covariance_3d(
                rotation, std::exp(scene.log_scale[i * 3 + 0]),
                std::exp(scene.log_scale[i * 3 + 1]), std::exp(scene.log_scale[i * 3 + 2]));
            const splat::Covariance2D cov = splat::project_covariance_2d(cov3d, x, y, z, fx, fy, 0.0f);
            const splat::CovarianceAxes axes = splat::covariance_axes(cov);
            const float sigma = std::sqrt(std::max(axes.lambda0, 0.0f));
            const float reach = 3.0f * (std::sqrt(std::max(axes.lambda0, 0.0f)) +
                                        std::sqrt(std::max(axes.lambda1, 0.0f)));
            counts.max_reach = std::max(counts.max_reach, reach);
            if (reach > 512.0f) ++counts.capped;
            ++counts.visible;
            if (sigma < 1.0f) ++counts.below_one;
            if (sigma < 0.5f) ++counts.below_half;
            counts.sigma.push_back(sigma);
        }
        std::sort(counts.sigma.begin(), counts.sigma.end());
        return counts;
    };

    const Counts headset = count(projection.intrinsics.fx, projection.intrinsics.fy,
                                 projection.intrinsics.cx, projection.intrinsics.cy, kEyeWidth,
                                 kEyeHeight);
    const Counts capture = count(capture_fx, loaded.value().meta.intrinsics.fy,
                                 loaded.value().meta.intrinsics.cx,
                                 loaded.value().meta.intrinsics.cy,
                                 loaded.value().meta.intrinsics.width,
                                 loaded.value().meta.intrinsics.height);
    REQUIRE(headset.visible > 0);
    REQUIRE(capture.visible > 0);
    auto median = [](const Counts& counts) {
        return counts.sigma.empty() ? 0.0f : counts.sigma[counts.sigma.size() / 2];
    };
    const double headset_below_one = static_cast<double>(headset.below_one) / headset.visible;
    const double capture_below_one = static_cast<double>(capture.below_one) / capture.visible;
    MESSAGE("PSVR2 eye: " << headset.visible << " visible, sigma median " << median(headset)
                          << " px, " << (100.0 * headset_below_one) << " % below 1 px, "
                          << (100.0 * headset.below_half / headset.visible)
                          << " % below 0.5 px, largest 3-sigma reach " << headset.max_reach
                          << " px, " << headset.capped << " splats over the 512 px cap, "
                          << headset.near << " inside 12 cm");
    MESSAGE("capture still: " << capture.visible << " visible, sigma median " << median(capture)
                              << " px, " << (100.0 * capture_below_one) << " % below 1 px, "
                              << (100.0 * capture.below_half / capture.visible)
                              << " % below 0.5 px");
    CHECK(median(headset) < median(capture));
    CHECK(headset_below_one > capture_below_one);
    CHECK(headset_below_one > 0.4);
#if defined(SPLAT_HAVE_VULKAN)
    // The shipped default is the Mip-Splatting 2D filter with compensation.
    const splat::RendererOptions defaults;
    CHECK(defaults.low_pass_eps == doctest::Approx(0.3f));
    CHECK(defaults.low_pass_compensate);
#endif
}

TEST_CASE("headset eye sequence does not flicker (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    const std::filesystem::path folder = scene_folder();
    if (!std::filesystem::exists(folder / "scene.psplat")) {
        MESSAGE("skipping: no scene at " << (folder / "scene.psplat"));
        return;
    }
    auto loaded = splat::read_scene_path(splat::Path((folder / "scene.psplat").string()));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    const Variant variants[] = {
        // name, eps2d px^2, compensate, near-fade end m, UNORM target, motion
        {"eps2d 0 (before)", 0.0f, true, 0.0f, false, 1.0f},
        {"eps2d 0.3 + compensation (now)", 0.3f, true, 0.08f, false, 1.0f},
        {"eps2d 0.3, no compensation", 0.3f, false, 0.08f, false, 1.0f},
        {"eps2d 0.1 + compensation", 0.1f, true, 0.08f, false, 1.0f},
        {"eps2d 0.5 + compensation", 0.5f, true, 0.08f, false, 1.0f},
        {"eps2d 1.0 + compensation", 1.0f, true, 0.08f, false, 1.0f},
        {"eps2d 0.3, hard near cull", 0.3f, true, 0.0f, false, 1.0f},
        {"eps2d 0.3, UNORM target", 0.3f, true, 0.08f, true, 1.0f},
        {"eps2d 0.3, no motion (control)", 0.3f, true, 0.08f, false, 0.0f},
    };

    std::error_code filesystem_error;
    const std::filesystem::path out = output_folder();
    std::filesystem::create_directories(out, filesystem_error);
    REQUIRE_MESSAGE(!filesystem_error, "cannot create " << out << ": " << filesystem_error.message());

    std::vector<Measurements> results(std::size(variants));
    for (size_t v = 0; v < std::size(variants); ++v) {
        std::string error;
        REQUIRE_MESSAGE(render_sequence(context, loaded.value().scene, variants[v], out,
                                       results[v], error),
                        variants[v].name << ": " << error);
    }

    // The PSNR column is per variant against the eps2d 0 first frame, the
    // still-render price of the filter. Compare over the full frame.
    for (size_t v = 1; v < std::size(variants); ++v) {
        results[v].psnr_vs_base = psnr(results[v].first_frame, results[0].first_frame);
        results[v].psnr_vs_fixed = psnr(results[v].first_frame, results[1].first_frame);
    }

    MESSAGE("variant                          | jerk | edge | pops % | grad | vs eps0 | vs eps.3 | project sort draw ms");
    for (size_t v = 0; v < std::size(variants); ++v) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "%-32s | %5.2f | %5.2f | %5.2f | %4.1f | %6.1f | %6.1f | %.2f %.2f %.2f",
                      variants[v].name, results[v].jerk, results[v].jerk_edge, results[v].pops,
                      results[v].gradient, results[v].psnr_vs_base, results[v].psnr_vs_fixed,
                      results[v].project_ms, results[v].sort_ms, results[v].draw_ms);
        MESSAGE(line);
    }

    // The fix: the low-pass with opacity compensation cuts edge flicker by a
    // wide margin and keeps the still frame close to the un-filtered render.
    const Measurements& base = results[0];
    const Measurements& fixed = results[1];
    CHECK_MESSAGE(fixed.jerk_edge < base.jerk_edge * 0.6,
                  "edge jerk " << fixed.jerk_edge << " vs " << base.jerk_edge);
    CHECK_MESSAGE(fixed.pops < base.pops, "pops " << fixed.pops << " % vs " << base.pops << " %");
    CHECK_MESSAGE(fixed.psnr_vs_base > 25.0, "still PSNR " << fixed.psnr_vs_base << " dB");
    // Compensation matters: without it the same dilation brightens the frame,
    // so the compensated variant must be closer to the current render.
    CHECK(fixed.psnr_vs_base > results[2].psnr_vs_base);
    // The colour space must stay sRGB (linear-light blending): the UNORM
    // variant is measurably further from the renderer's own blend.
    CHECK(results[7].jerk_edge > fixed.jerk_edge);
    // Time: the filter is a few ALU ops in the projection pass.
    CHECK(fixed.draw_ms < base.draw_ms * 1.15 + 0.5);
    CHECK(fixed.project_ms < base.project_ms * 1.5 + 0.2);

    CHECK(results[0].jerk_edge > 0.0);
#endif
}

// The price of the filter, measured against the source photo. SHARP fitted its
// Gaussians to gsplat's unfiltered ("classic", eps2d 0) rasteriser at the
// capture resolution, so a low-pass moves the still slightly away from the
// photo: the filtered splats no longer reproduce the sharper, aliased image the
// model was optimised against. This test pins that cost (about 1.3 dB at
// eps2d 0.3) and shows it growing for stronger filters, which is why 0.3 is the
// default: it is the largest value that keeps the still within 35 dB of the
// unfiltered render and 1.5 dB of the photo, while cutting the headset's edge
// flicker by two thirds.
TEST_CASE("capture-pose still against the source photo (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    const std::filesystem::path folder = scene_folder();
    if (!std::filesystem::exists(folder / "scene.psplat")) {
        MESSAGE("skipping: no scene at " << (folder / "scene.psplat"));
        return;
    }
    auto loaded = splat::read_scene_path(splat::Path((folder / "scene.psplat").string()));
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    const splat::Intrinsics& capture = loaded.value().meta.intrinsics;
    REQUIRE(capture.fx > 0.0f);
    auto photo_result = splat::decode_image(splat::Path((folder / "original.jpg").string()));
    REQUIRE_MESSAGE(photo_result.ok(), photo_result.message());
    const splat::Image& photo = photo_result.value();
    REQUIRE(photo.width == static_cast<int>(capture.width));
    REQUIRE(photo.height == static_cast<int>(capture.height));

    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    const float eps[] = {0.0f, 0.1f, 0.3f, 0.5f, 1.0f};
    double quality[std::size(eps)] = {0.0};
    for (size_t i = 0; i < std::size(eps); ++i) {
        splat::RendererOptions options;
        options.shader_dir = SPLAT_SHADER_DIR;
        options.eye_width = capture.width;
        options.eye_height = capture.height;
        options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
        options.intrinsics = {capture.fx, capture.fy, capture.cx, capture.cy};
        options.capture_intrinsics = {capture.fx, capture.fy, capture.cx, capture.cy};
        options.capture_width = capture.width;
        options.capture_height = capture.height;
        options.low_pass_eps = eps[i];
        splat::Image still;
        std::string error;
        REQUIRE_MESSAGE(render_still(context, loaded.value().scene, options, still, error), error);
        quality[i] = psnr(still, photo);
        MESSAGE("eps2d " << eps[i] << ": " << quality[i] << " dB against the photo");
    }
    CHECK(quality[0] > 25.0);
    // eps2d 0.3 costs at most 1.5 dB against the source photo, and much less
    // than the over-blurred eps2d 1.0.
    CHECK(quality[2] > quality[0] - 1.5);
    CHECK(quality[2] > quality[4]);
#endif
}

// Near-plane crossing: the hard 5 cm cull makes a splat appear at full
// strength the instant the head moves past it (the second half of "edges
// blink" when leaning into geometry). The near fade ramps alpha up from the
// near plane so the crossing is continuous. One white Gaussian on the axis,
// viewed from z = 4.9 cm to 10 cm.
TEST_CASE("near-plane crossing fades instead of popping (MoltenVK)") {
#if !defined(SPLAT_HAVE_VULKAN)
    MESSAGE("skipping: built without Vulkan");
    return;
#else
    auto context_result = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context_result.ok()) {
        MESSAGE("skipping: " << context_result.message());
        return;
    }
    splat::VulkanContext& context = *context_result.value();

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, 1.0f};
    const float log_sigma = std::log(0.002f);
    scene.log_scale = {log_sigma, log_sigma, log_sigma};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    const float white_dc = (1.0f - 0.5f) / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {12.0f};

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 128;
    options.eye_height = 128;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {200.0f, 200.0f, 64.0f, 64.0f};

    const float depths[] = {0.049f, 0.052f, 0.060f, 0.070f, 0.100f};
    double hard[std::size(depths)] = {0.0};
    double faded[std::size(depths)] = {0.0};
    for (size_t i = 0; i < std::size(depths); ++i) {
        for (int pass = 0; pass < 2; ++pass) {
            splat::RendererOptions trial = options;
            trial.near_fade_end = pass == 0 ? 0.0f : 0.08f;
            // The splat sits at the measured depth in front of the capture
            // pose camera (the camera stays at the origin: moving it would
            // change the view depth by the same amount).
            splat::SplatScene placed = scene;
            placed.position[2] = depths[i];
            const splat::FrameView view;
            std::string error;
            splat::Image image;
            const bool ok = [&] {
                auto renderer_result = splat::SplatRenderer::create(context, trial, placed);
                if (!renderer_result.ok()) {
                    error = renderer_result.message();
                    return false;
                }
                std::unique_ptr<splat::SplatRenderer> renderer = renderer_result.take();
                auto image_result = splat::create_vulkan_color_image(
                    context, trial.target_format, renderer->target_extent(),
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                if (!image_result.ok()) {
                    error = image_result.message();
                    return false;
                }
                splat::VulkanImage target_image = image_result.take();
                splat::RenderTarget target;
                target.image = target_image.image;
                target.view = target_image.view;
                target.extent = renderer->target_extent();
                target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                const splat::Status status = renderer->render(view, target, true);
                auto pixels = renderer->read_image();
                splat::destroy_vulkan_image(context, target_image);
                if (!status.ok()) {
                    error = status.message();
                    return false;
                }
                if (!pixels.ok()) {
                    error = pixels.message();
                    return false;
                }
                image = std::move(pixels.value());
                return true;
            }();
            REQUIRE_MESSAGE(ok, error);
            // Mean luminance over a 96x96 box around the axis, which holds
            // the whole splat at every depth here.
            double sum = 0.0;
            for (int y = 16; y < 112; ++y) {
                for (int x = 16; x < 112; ++x) {
                    const uint8_t* p = image.pixel(x, y);
                    sum += (p[0] + p[1] + p[2]) / 3.0;
                }
            }
            (pass == 0 ? hard : faded)[i] = sum / (96.0 * 96.0);
        }
    }
    for (size_t i = 0; i < std::size(depths); ++i) {
        MESSAGE("z " << depths[i] << " m: hard cull mean " << hard[i] << ", near fade " << faded[i]);
    }
    const double backdrop = hard[0];
    REQUIRE(hard[1] > backdrop + 0.5);
    // The hard cull pops the whole splat in between two frames (5.2 cm is
    // already at full strength: the step is the entire splat).
    CHECK(hard[1] - hard[0] > 10.0 * (faded[1] - faded[0]));
    // Through the band alpha follows smoothstep(0.05, 0.08, z): the fade is
    // continuous and reaches full strength exactly at its end.
    for (size_t i = 1; i < std::size(depths); ++i) {
        const double t = std::clamp((depths[i] - 0.05) / (0.08 - 0.05), 0.0, 1.0);
        const double expected = t * t * (3.0 - 2.0 * t);
        const double measured = (faded[i] - backdrop) / (hard[i] - backdrop);
        MESSAGE("z " << depths[i] << " m: alpha " << measured << " (smoothstep " << expected
                     << ")");
        CHECK(measured == doctest::Approx(expected).epsilon(0.08));
    }
    // Outside the band the fade is a no-op.
    CHECK(faded[4] == doctest::Approx(hard[4]).epsilon(1e-6));
#endif
}
