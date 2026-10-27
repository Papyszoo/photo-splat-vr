#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

// Linear -> sRGB-encoded 8-bit, the value an sRGB/UNORM readback stores for a
// linear clear/backdrop of `value`.
int encoded(float value) {
    const float s = value <= 0.0031308f ? 12.92f * value
                                        : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(s * 255.0f));
}

splat::SplatScene single_splat(float sigma) {
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, 1.0f};
    scene.log_scale = {std::log(sigma), std::log(sigma), std::log(sigma)};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    const float white_dc = 0.8f / 0.28209479177387814f;
    scene.sh_dc = {white_dc, white_dc, white_dc};
    scene.opacity_logit = {12.0f};
    return scene;
}

// 3 sigma at fx = 50 px, z = 1 m covers the whole 64x64 viewport.
splat::SplatScene big_splat() { return single_splat(0.5f); }

// Fronto-parallel plane at z = 1 m, a grid of opaque 3 px Gaussians (sigma
// 3 cm at fx = 200 px). Splats left of `step_x` are black, the rest white, so
// the rendered frame has one sharp vertical edge whose stereo disparity is the
// scene disparity near it. `step_x` is in world metres at z = 1.
splat::SplatScene stepped_plane(float step_x) {
    constexpr float kSigma = 0.03f;
    constexpr float kSpacing = 0.03f;
    const float white_dc = 0.8f / 0.28209479177387814f;
    const float black_dc = -0.5f / 0.28209479177387814f;

    splat::SplatScene scene;
    const int nx = 81;  // x -0.8 .. 1.6 m
    const int ny = 47;  // y -0.7 .. 0.7 m
    scene.resize(static_cast<size_t>(nx) * ny);
    size_t i = 0;
    for (int ix = 0; ix < nx; ++ix) {
        const float x = -0.8f + kSpacing * ix;
        const float dc = x < step_x ? black_dc : white_dc;
        for (int iy = 0; iy < ny; ++iy) {
            const float y = -0.7f + kSpacing * iy;
            scene.position[i * 3 + 0] = x;
            scene.position[i * 3 + 1] = y;
            scene.position[i * 3 + 2] = 1.0f;
            scene.log_scale[i * 3 + 0] = std::log(kSigma);
            scene.log_scale[i * 3 + 1] = std::log(kSigma);
            scene.log_scale[i * 3 + 2] = std::log(kSigma);
            scene.rotation[i * 4 + 0] = 1.0f;
            scene.rotation[i * 4 + 1] = 0.0f;
            scene.rotation[i * 4 + 2] = 0.0f;
            scene.rotation[i * 4 + 3] = 0.0f;
            scene.sh_dc[i * 3 + 0] = dc;
            scene.sh_dc[i * 3 + 1] = dc;
            scene.sh_dc[i * 3 + 2] = dc;
            scene.opacity_logit[i] = 12.0f;
            ++i;
        }
    }
    return scene;
}

// Red-channel profile of one eye, averaged over a band of rows. The step and
// the fade are vertical, so the average keeps their sub-pixel position while
// removing the per-splat texture noise.
std::vector<double> eye_profile(const splat::Image& frame, int eye, int eye_width, int y0,
                                int y1) {
    std::vector<double> out(static_cast<size_t>(eye_width), 0.0);
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < eye_width; ++x) {
            out[static_cast<size_t>(x)] +=
                frame.pixel(eye * eye_width + x, y)[0] / static_cast<double>(y1 - y0);
        }
    }
    return out;
}

// First x in [x0, x1) where the profile crosses `level`, linearly interpolated.
// Returns -1 when the profile never crosses.
double crossing(const std::vector<double>& profile, int x0, int x1, double level) {
    for (int x = x0; x + 1 <= x1; ++x) {
        const double a = profile[static_cast<size_t>(x)] - level;
        const double b = profile[static_cast<size_t>(x + 1)] - level;
        if (a != b && ((a < 0.0) != (b < 0.0))) {
            return x + a / (a - b);
        }
    }
    return -1.0;
}

}  // namespace

TEST_CASE("comfort final blend fades the whole frame to the backdrop") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 64;
    options.eye_height = 64;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};
    options.backdrop[0] = 0.5f;
    options.backdrop[1] = 0.25f;
    options.backdrop[2] = 0.125f;

    auto renderer = splat::SplatRenderer::create(device, options, big_splat());
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

    auto render_at = [&](float fade) {
        splat::FrameView view;
        view.comfort_fade = fade;
        const splat::Status status = renderer.value()->render(view, target, true);
        REQUIRE_MESSAGE(status.ok(), status.message());
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer.value()->read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        return pixels.take();
    };

    // fade = 0: the final blend replaces the splat image with the backdrop
    // everywhere, including the corners and the centre.
    const splat::Image faded = render_at(0.0f);
    const int expected[3] = {encoded(0.5f), encoded(0.25f), encoded(0.125f)};
    for (const auto& point : {std::pair<int, int>{0, 0}, std::pair<int, int>{32, 32},
                              std::pair<int, int>{63, 63}}) {
        const uint8_t* p = faded.pixel(point.first, point.second);
        CHECK(std::abs(static_cast<int>(p[0]) - expected[0]) <= 1);
        CHECK(std::abs(static_cast<int>(p[1]) - expected[1]) <= 1);
        CHECK(std::abs(static_cast<int>(p[2]) - expected[2]) <= 1);
    }

    // fade = 1: the pass is a no-op and the white splat is visible again.
    const splat::Image visible = render_at(1.0f);
    const uint8_t* centre = visible.pixel(32, 32);
    CHECK(centre[0] > 240);
    CHECK(centre[1] > 240);
    CHECK(centre[2] > 240);

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("view rays outside the capture image fade to the backdrop") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 64;
    options.eye_height = 64;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};
    // The capture image covers the left half of the rendered frame only, with
    // a soft edge a quarter of its width wide beyond the border: the right
    // half is extrapolated geometry and must fade out. The default 2 % inner
    // margin is under a pixel at this size, so the border itself is still at
    // full opacity here (the inner ramp has its own test).
    options.capture_intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};
    options.capture_width = 32;
    options.capture_height = 64;
    options.capture_margin = 0.25f;
    options.backdrop[0] = 0.5f;
    options.backdrop[1] = 0.25f;
    options.backdrop[2] = 0.125f;

    auto renderer = splat::SplatRenderer::create(device, options, single_splat(1.0f));
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
    // Radial fade is 1; only the capture frustum decides here.
    REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, target, true).ok(), "render");
    auto pixels = renderer.value()->read_image();
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());
    const splat::Image frame = pixels.take();

    const int backdrop[3] = {encoded(0.5f), encoded(0.25f), encoded(0.125f)};
    const uint8_t* inside = frame.pixel(16, 32);
    CHECK(inside[0] > 210);  // white splat, fully inside the capture image

    // x = 60 -> capture u = 1.89, well past the 0.25 margin: pure backdrop.
    const uint8_t* outside = frame.pixel(60, 32);
    for (int c = 0; c < 3; ++c) CHECK(std::abs(static_cast<int>(outside[c]) - backdrop[c]) <= 1);

    // The edge is soft: mid-margin is a blend, not a hard line.
    const uint8_t* edge = frame.pixel(36, 32);
    CHECK(edge[0] < inside[0]);
    CHECK(edge[0] > backdrop[0] + 5);

    // And it is monotone across the transition (no ringing, one band).
    int previous = 256;
    for (int x = 28; x <= 46; ++x) {
        const int red = frame.pixel(x, 32)[0];
        CHECK(red <= previous + 1);
        previous = red;
    }

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("capture-frustum fade starts inside the capture border") {
    // The default inner margin is 2 % of the capture image size: 25.6 px at a
    // 1280-wide photo, the width of the washed-out, speckled strip SHARP's weak
    // border Gaussians leave just inside the frame.
    CHECK(splat::RendererOptions{}.capture_inner_margin == doctest::Approx(0.02f));

    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 64;
    options.eye_height = 64;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};
    // Capture image 32 px wide, so its border is at x = 32 of the 64 px render
    // and both margins are 8 px: the ramp covers x = 24 .. 40.
    options.capture_intrinsics = {50.0f, 50.0f, 32.0f, 32.0f};
    options.capture_width = 32;
    options.capture_height = 64;
    options.capture_margin = 0.25f;
    options.capture_inner_margin = 0.25f;
    // Neutral 0.18 grey, encoded 118: the contrast between the white splat and
    // the backdrop is then 137 levels, so the fade is easy to read.
    options.backdrop[0] = 0.18f;
    options.backdrop[1] = 0.18f;
    options.backdrop[2] = 0.18f;

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, VkExtent2D{options.eye_width, options.eye_height},
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = VkExtent2D{options.eye_width, options.eye_height};
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    auto render_with = [&](float inner_margin) {
        splat::RendererOptions renderer_options = options;
        renderer_options.capture_inner_margin = inner_margin;
        auto renderer =
            splat::SplatRenderer::create(device, renderer_options, single_splat(1.0f));
        REQUIRE_MESSAGE(renderer.ok(), renderer.message());
        target.extent = renderer.value()->target_extent();
        REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, target, true).ok(), "render");
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer.value()->read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        return pixels.take();
    };

    // The pre-F7 ramp (starts at the border) as the reference for the strip.
    const splat::Image reference = render_with(0.0f);
    const splat::Image inner = render_with(0.25f);

    // Deep inside the photo nothing changed: the inner margin only reaches the
    // 8 px next to the border.
    for (int x = 8; x <= 20; ++x) {
        const int a = reference.pixel(x, 32)[0];
        const int b = inner.pixel(x, 32)[0];
        CHECK(std::abs(b - a) <= 1);
    }
    CHECK(inner.pixel(10, 32)[0] > 240);

    // All four borders of the photo are weak, so the left one fades too: its
    // first column sits 0.5 px inside the border, already inside the ramp.
    CHECK(inner.pixel(0, 32)[0] < reference.pixel(0, 32)[0] - 20);

    // At the border the old ramp was still at full opacity; the new one is well
    // into the fade, so the weakest border splats now go to the backdrop too.
    for (int x = 29; x <= 32; ++x) {
        CHECK(inner.pixel(x, 32)[0] < reference.pixel(x, 32)[0] - 20);
    }
    // The fade never brightens a pixel and never starts before x = 24 (the
    // ramp start, 25 % inside the capture image). Only the right border is
    // scanned: the left one is inside its own ramp.
    int first_dimmed = -1;
    for (int x = 16; x < 48; ++x) {
        const int a = reference.pixel(x, 32)[0];
        const int b = inner.pixel(x, 32)[0];
        CHECK(b <= a + 1);
        if (first_dimmed < 0 && b < a - 2) first_dimmed = x;
    }
    MESSAGE("full opacity ends at x = " << first_dimmed << " (ramp start 24)");
    CHECK(first_dimmed >= 24);
    CHECK(first_dimmed <= 26);

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("stereo capture-frustum fade edge has the scene disparity") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    constexpr uint32_t kEyeWidth = 320;
    constexpr uint32_t kEyeHeight = 240;
    constexpr float kFocal = 200.0f;
    // The capture camera is the same lens with a narrower (192 px) sensor and
    // its principal point in the middle: the rendered view extends 128 px past
    // its right border, where the capture-frustum fade lives.
    constexpr float kCaptureWidth = 192.0f;

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {kFocal, kFocal, 0.5f * kEyeWidth, 0.5f * kEyeHeight};
    options.capture_intrinsics = {kFocal, kFocal, 0.5f * kCaptureWidth,
                                  0.5f * kEyeHeight};
    options.capture_width = static_cast<uint32_t>(kCaptureWidth);
    options.capture_height = kEyeHeight;
    options.capture_margin = 0.03125f;  // 40 px at a 1280-wide capture image
    options.backdrop[0] = 0.18f;
    options.backdrop[1] = 0.18f;
    options.backdrop[2] = 0.18f;

    // 60 % of the default 0.25 m comfort radius: the head sits 0.15 m to the
    // right, the eyes at 0.1185 m and 0.1815 m. The step is 0.33 m from the
    // capture camera and the fade edge around 0.5 m, both on the same plane, so
    // their stereo disparity must match (fx * IPD / z = 12.6 px at z = 1 m).
    constexpr float kHeadX = 0.15f;
    auto renderer = splat::SplatRenderer::create(device, options, stepped_plane(0.33f));
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

    splat::FrameView view;
    view.midpoint.position[0] = kHeadX;
    view.comfort_fade = 1.0f;  // only the capture-frustum term is under test
    REQUIRE_MESSAGE(renderer.value()->render(view, target, true).ok(), "render");
    auto pixels = renderer.value()->read_image();
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());
    const splat::Image frame = pixels.take();

    const std::vector<double> left =
        eye_profile(frame, 0, kEyeWidth, static_cast<int>(kEyeHeight) / 2 - 24,
                    static_cast<int>(kEyeHeight) / 2 + 24);
    const std::vector<double> right =
        eye_profile(frame, 1, kEyeWidth, static_cast<int>(kEyeHeight) / 2 - 24,
                    static_cast<int>(kEyeHeight) / 2 + 24);

    // The black -> white step, fully inside the capture image in both eyes.
    // 127 is halfway between the black (0) and white (255) plane...
    const double step_left = crossing(left, 160, 215, 127.0);
    const double step_right = crossing(right, 160, 215, 127.0);
    // ...and 186 is halfway between the white plane and the encoded 0.18
    // linear backdrop (118), i.e. the 50 % point of the fade.
    const double fade_left = crossing(left, 215, 300, 186.0);
    const double fade_right = crossing(right, 215, 300, 186.0);
    REQUIRE(step_left >= 0.0);
    REQUIRE(step_right >= 0.0);
    REQUIRE(fade_left >= 0.0);
    REQUIRE(fade_right >= 0.0);

    const double scene_disparity = step_left - step_right;
    const double edge_disparity = fade_left - fade_right;
    MESSAGE("scene disparity " << scene_disparity << " px, fade edge disparity "
                               << edge_disparity << " px");

    // The scene really has parallax at the fade region...
    CHECK(scene_disparity > 8.0);
    // ...and the fade edge sits on it, not on a fixed screen x. The old
    // midpoint version put both edges at the same x (0 px disparity).
    CHECK(std::abs(edge_disparity - scene_disparity) <= 2.0);

    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("comfort final blend is skipped without Vulkan") { CHECK(true); }

#endif
