#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "render/project_math.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

// Intensity centroid in pixel-centre coordinates (pixel i spans [i, i+1)).
bool luminance_centroid(const splat::Image& image, double& x, double& y) {
    float peak = 0.0f;
    for (size_t i = 0; i < image.pixel_count(); ++i) {
        const uint8_t* p = image.rgb.data() + i * 3;
        peak = std::max(peak, (p[0] + p[1] + p[2]) / 3.0f);
    }
    if (peak < 1.0f) return false;
    const float threshold = 0.2f * peak;

    double sum = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* p = image.pixel(px, py);
            const float value = (p[0] + p[1] + p[2]) / 3.0f;
            if (value < threshold) continue;
            sum += value;
            sx += (px + 0.5) * value;
            sy += (py + 0.5) * value;
        }
    }
    if (sum <= 0.0) return false;
    x = sx / sum;
    y = sy / sum;
    return true;
}

// Second moments of the alpha footprint, about the intensity centroid, in
// pixels^2 and pixel offset coordinates: [xx, xy, yy].
struct Moments {
    double mass = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
};

struct CpuFootprint {
    Moments moments;
    double max_alpha = 0.0;
};

// The pixel centres the shader's quad covers, with the Gaussian alpha the
// fragment shader computes: the quad corners sit at +/-3 semi-axes, and the
// fragment's `position` is the displacement from the centre measured in
// semi-axis lengths.
CpuFootprint rasterize_on_cpu(const splat::CovarianceAxes& axes, double centre_x,
                              double centre_y, double opacity, int width, int height) {
    CpuFootprint out;
    const double len0 = axes.axis0[0] * axes.axis0[0] + axes.axis0[1] * axes.axis0[1];
    const double len1 = axes.axis1[0] * axes.axis1[0] + axes.axis1[1] * axes.axis1[1];
    if (!(len0 > 0.0) || !(len1 > 0.0)) return out;

    // Quad extent in pixels: 3 * (|axis0| + |axis1|) per screen axis.
    const double reach_x = 3.0 * (std::abs(axes.axis0[0]) + std::abs(axes.axis1[0]));
    const double reach_y = 3.0 * (std::abs(axes.axis0[1]) + std::abs(axes.axis1[1]));
    const int x0 = std::max(0, static_cast<int>(std::floor(centre_x - reach_x)) - 1);
    const int x1 = std::min(width - 1, static_cast<int>(std::ceil(centre_x + reach_x)) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(centre_y - reach_y)) - 1);
    const int y1 = std::min(height - 1, static_cast<int>(std::ceil(centre_y + reach_y)) + 1);

    std::vector<std::pair<int, int>> pixels;
    std::vector<double> alphas;
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            const double dx = px + 0.5 - centre_x;
            const double dy = py + 0.5 - centre_y;
            const double u = (dx * axes.axis0[0] + dy * axes.axis0[1]) / len0;
            const double v = (dx * axes.axis1[0] + dy * axes.axis1[1]) / len1;
            if (std::abs(u) > 3.0 || std::abs(v) > 3.0) continue;  // outside the quad
            double alpha = opacity * std::exp(-0.5 * (u * u + v * v));
            if (alpha > 1.0) alpha = 1.0;
            if (alpha < 1.0 / 512.0) continue;
            pixels.emplace_back(px, py);
            alphas.push_back(alpha);
            out.moments.mass += alpha;
            out.moments.cx += alpha * (px + 0.5);
            out.moments.cy += alpha * (py + 0.5);
            out.max_alpha = std::max(out.max_alpha, alpha);
        }
    }
    if (!(out.moments.mass > 0.0)) return out;
    const double cx = out.moments.cx / out.moments.mass;
    const double cy = out.moments.cy / out.moments.mass;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const double ox = pixels[i].first + 0.5 - cx;
        const double oy = pixels[i].second + 0.5 - cy;
        out.moments.xx += alphas[i] * ox * ox;
        out.moments.xy += alphas[i] * ox * oy;
        out.moments.yy += alphas[i] * oy * oy;
    }
    out.moments.cx = cx;
    out.moments.cy = cy;
    out.moments.xx /= out.moments.mass;
    out.moments.xy /= out.moments.mass;
    out.moments.yy /= out.moments.mass;
    return out;
}

// Measures the same moments from a rendered image. The splat is a single
// white-ish Gaussian, so intensity is proportional to alpha.
bool image_moments(const splat::Image& image, Moments& out) {
    double mass = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* p = image.pixel(px, py);
            const double value = (p[0] + p[1] + p[2]) / (3.0 * 255.0);
            if (value <= 0.0) continue;
            mass += value;
            sx += value * (px + 0.5);
            sy += value * (py + 0.5);
        }
    }
    if (mass <= 0.0) return false;
    const double cx = sx / mass;
    const double cy = sy / mass;
    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    for (int py = 0; py < image.height; ++py) {
        for (int px = 0; px < image.width; ++px) {
            const uint8_t* p = image.pixel(px, py);
            const double value = (p[0] + p[1] + p[2]) / (3.0 * 255.0);
            if (value <= 0.0) continue;
            const double ox = px + 0.5 - cx;
            const double oy = py + 0.5 - cy;
            xx += value * ox * ox;
            xy += value * ox * oy;
            yy += value * oy * oy;
        }
    }
    out = Moments{};
    out.mass = mass;
    out.cx = cx;
    out.cy = cy;
    out.xx = xx / mass;
    out.xy = xy / mass;
    out.yy = yy / mass;
    return true;
}

// Renders one Gaussian at the capture pose and returns the image.
splat::Result<splat::Image> render_one(const splat::SplatScene& scene,
                                       const splat::RendererOptions& options,
                                       splat::VulkanContext& device) {
    using Result = splat::Result<splat::Image>;
    auto renderer = splat::SplatRenderer::create(device, options, scene);
    if (!renderer.ok()) return Result::failure(renderer.message());

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, renderer.value()->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!image.ok()) return Result::failure(image.message());

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.value()->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    const splat::Status status = renderer.value()->render(splat::FrameView{}, target, true);
    if (!status.ok()) return Result::failure(status.message());
    auto pixels = renderer.value()->read_image();
    splat::destroy_vulkan_image(device, image.value());
    return pixels;
}

}  // namespace

TEST_CASE("one Gaussian projects to the expected pixel") {
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.1f, -0.05f, 1.0f};
    // 2 cm at 1 m with f = 100 px is a 2 px sigma, so the 3-sigma quad covers
    // pixel centres. A sub-pixel Gaussian is invisible with SHARP's zero
    // low-pass term (gsplat skips radii below one pixel too).
    const float log_sigma = std::log(0.02f);
    scene.log_scale = {log_sigma, log_sigma, log_sigma};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    const float white_dc = 0.5f / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {12.0f};

    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    // CI runs on lavapipe, whose subgroup size does not meet the renderer's
    // device requirements; skip there instead of failing the suite.
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 128;
    options.eye_height = 96;
    options.stereo = false;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    // These tests measure the splat footprint against a black background; the
    // viewer's neutral comfort backdrop would otherwise dominate the centroid.
    options.backdrop[0] = 0.0f;
    options.backdrop[1] = 0.0f;
    options.backdrop[2] = 0.0f;
    options.intrinsics = {100.0f, 100.0f, 64.0f, 48.0f};
    // Pin SHARP's own projection: no anti-aliasing low-pass here, the CPU
    // reference below is projected with eps = 0.
    options.low_pass_eps = 0.0f;

    auto renderer = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, renderer.value()->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.value()->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    const splat::Status status =
        renderer.value()->render(splat::FrameView{}, target, true);
    REQUIRE_MESSAGE(status.ok(), status.message());
    REQUIRE(renderer.value()->last_visible_count() == 1);

    auto pixels = renderer.value()->read_image();
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());

    double x = 0.0;
    double y = 0.0;
    REQUIRE(luminance_centroid(pixels.value(), x, y));

    // cx + fx * X / Z = 64 + 100 * 0.1 = 74, cy + fy * Y / Z = 48 - 5 = 43.
    CHECK(x == doctest::Approx(74.0).epsilon(0.01));
    CHECK(y == doctest::Approx(43.0).epsilon(0.01));

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("an off-axis rotated anisotropic Gaussian projects to the CPU conic") {
    // Same Gaussian as the CPU test in render_projection_math_test.cpp: rotated
    // about x and z, log-scale ratios 4:2:1, off axis in x and y. The rendered
    // footprint must match the conic computed by project_math.h, which pins the
    // shader's rotation and Jacobian to the CPU reference.
    const float angle_z = 30.0f * 3.14159265358979323846f / 180.0f;
    const float angle_x = 40.0f * 3.14159265358979323846f / 180.0f;
    const float cz = std::cos(0.5f * angle_z);
    const float sz = std::sin(0.5f * angle_z);
    const float cx_quat = std::cos(0.5f * angle_x);
    const float sx_quat = std::sin(0.5f * angle_x);
    const float qw = cz * cx_quat;
    const float qx = cz * sx_quat;
    const float qy = sz * sx_quat;
    const float qz = -sz * cx_quat;

    const float scales[3] = {0.07f, 0.035f, 0.02f};
    const float position[3] = {0.31f, -0.19f, 1.4f};
    const float fx = 200.0f;
    const float fy = 200.0f;
    const float cx = 100.0f;
    const float cy = 100.0f;
    const uint32_t width = 200;
    const uint32_t height = 200;

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {position[0], position[1], position[2]};
    for (int i = 0; i < 3; ++i) scene.log_scale[i] = std::log(scales[i]);
    scene.rotation = {qw, qx, qy, qz};
    const float white_dc = (0.8f - 0.5f) / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {0.0f};  // opacity 0.5, so the fragment never clamps

    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    // CI runs on lavapipe, whose subgroup size does not meet the renderer's
    // device requirements; skip there instead of failing the suite.
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = width;
    options.eye_height = height;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    // These tests measure the splat footprint against a black background; the
    // viewer's neutral comfort backdrop would otherwise dominate the centroid.
    options.backdrop[0] = 0.0f;
    options.backdrop[1] = 0.0f;
    options.backdrop[2] = 0.0f;
    options.intrinsics = {fx, fy, cx, cy};
    // The footprint is compared against project_math.h at eps = 0.
    options.low_pass_eps = 0.0f;

    auto pixels = render_one(scene, options, device);
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());

    // CPU reference for the same splat.
    const splat::Mat3 rotation = splat::quaternion_to_rotation(qw, qx, qy, qz);
    const splat::Mat3 cov3d = splat::gaussian_covariance_3d(rotation, scales[0], scales[1], scales[2]);
    const splat::Covariance2D cov2d = splat::project_covariance_2d(
        cov3d, position[0], position[1], position[2], fx, fy, 0.0f);
    const splat::CovarianceAxes axes = splat::covariance_axes(cov2d);
    const double centre_x = fx * position[0] / position[2] + cx;
    const double centre_y = fy * position[1] / position[2] + cy;

    const CpuFootprint expected =
        rasterize_on_cpu(axes, centre_x, centre_y, 0.5, static_cast<int>(width),
                         static_cast<int>(height));
    REQUIRE(expected.moments.mass > 1.0);
    // The footprint is a real ellipse, not a dot or the whole screen.
    const double sigma0 = std::sqrt(axes.lambda0);
    const double sigma1 = std::sqrt(axes.lambda1);
    CHECK(sigma0 > 6.0);
    CHECK(sigma1 > 3.0);
    CHECK(std::abs(axes.axis0[0] * axes.axis1[1] - axes.axis0[1] * axes.axis1[0]) > 1.0);

    Moments measured;
    REQUIRE(image_moments(pixels.value(), measured));

    // Centroid: pinhole projection of the centre.
    CHECK(measured.cx == doctest::Approx(expected.moments.cx).epsilon(0.005));
    CHECK(measured.cy == doctest::Approx(expected.moments.cy).epsilon(0.005));

    // Second moments: the projected conic's orientation and extent. Measured
    // agreement is better than 0.2 %; 2 % leaves room for fp16 rounding and
    // driver differences. Both historical bugs are far outside it: a
    // transposed rotation flips xy and moves xx by 14 %, a transposed Jacobian
    // moves xx by 11 % and xy by 23 %.
    CAPTURE(measured.xx);
    CAPTURE(expected.moments.xx);
    CHECK(measured.xx == doctest::Approx(expected.moments.xx).epsilon(0.02));
    CHECK(measured.xy == doctest::Approx(expected.moments.xy).epsilon(0.02));
    CHECK(measured.yy == doctest::Approx(expected.moments.yy).epsilon(0.02));

    // A transposed rotation or Jacobian changes the ellipse enough to fail
    // these; compare against the eigenvalues of the conic as a sanity check on
    // the scale of the tolerance.
    const double trace = expected.moments.xx + expected.moments.yy;
    CHECK(trace > 40.0);
}

#else

TEST_CASE("splat projection is skipped without Vulkan") { CHECK(true); }

#endif
