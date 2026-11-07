// 0.6.0, "one menu, two views": the TV's picture of a VR session.
//
// The Hmd2 mirror can only show the eye buffers, so the title builds the TV's
// picture itself: the left eye's centre 16:9 crop of the scene taken BEFORE the
// overlay pass (so the VR window, the scene label and the HUD are not in it),
// with the same menu canvas drawn flat over it while the menu is open.
//
// Parts:
//   - the crop maths (centred on the optical axis, 16:9, inside the eye) on the
//     real PSVR2 numbers, and the TV camera's quads landing on exact pixels;
//   - the bounded drop queue between the render loop and the TV worker (never
//     waits, newest wins, bounded, closes cleanly), including two real threads;
//   - GPU (skips without a Vulkan device): a stereo frame with a VR window quad
//     in the eyes' overlay list and a flat panel in the TV list. The TV picture
//     has the panel and none of the window; the eyes still have the window; a
//     frame that did not ask for a TV picture leaves its slot alone; and the
//     same holds for the gamma-space (UNORM) blend target.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

#include "render/camera.h"
#include "render/hmd_view.h"
#include "render/overlay_quad.h"
#include "render/tv_slots.h"
#include "render/tv_view.h"

#if defined(SPLAT_HAVE_VULKAN)

#include "common/image.h"
#include "render/splat_renderer.h"
#include "render/ui_panel.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

#endif  // SPLAT_HAVE_VULKAN

// ---- the crop ----------------------------------------------------------------

TEST_CASE("the TV crop is the largest 16:9 centred on the left eye's optical axis") {
    // The PSVR2 numbers: 1984x2048 per eye, FOV tangents out 1.842 / in 0.947 /
    // top = bottom 1.329. The left eye's axis is far right of the image centre.
    const splat::HeadsetProjection projection =
        splat::hmd_projection({1.842f, 0.947f, 1.329f, 1.329f}, 1984, 2048);
    const float cx = projection.intrinsics.cx + projection.eye_shift[0];
    const float cy = projection.intrinsics.cy;
    CHECK(cx == doctest::Approx(1984.0f * 1.842f / (1.842f + 0.947f)).epsilon(1e-4));

    const splat::TvCrop crop = splat::tv_crop(cx, cy, 1984, 2048);
    REQUIRE(crop.valid());
    // Inside the eye image, never outside it.
    CHECK(crop.x0 >= 0);
    CHECK(crop.y0 >= 0);
    CHECK(crop.x1 <= 1984);
    CHECK(crop.y1 <= 2048);
    // 16:9 (to a pixel of rounding) ...
    CHECK(static_cast<double>(crop.width()) / crop.height() ==
          doctest::Approx(16.0 / 9.0).epsilon(0.004));
    // ... centred on the axis, not on the image: the left eye's nasal side is the
    // short one (1984 - 1310 = 674 px), so that is the half width.
    CHECK(0.5 * (crop.x0 + crop.x1) == doctest::Approx(cx).epsilon(1e-3));
    CHECK(0.5 * (crop.y0 + crop.y1) == doctest::Approx(cy).epsilon(1e-3));
    CHECK(crop.width() == doctest::Approx(2.0 * (1984.0 - cx)).epsilon(0.002));
    // An image-centred crop would sit 318 px to the temporal side of the axis.
    CHECK(std::abs(0.5 * (crop.x0 + crop.x1) - 992.0) > 300.0);

    // Symmetric eye (the desktop's), 16:9: the whole image.
    const splat::TvCrop whole = splat::tv_crop(640.0f, 360.0f, 1280, 720);
    CHECK(whole.x0 == 0);
    CHECK(whole.y0 == 0);
    CHECK(whole.x1 == 1280);
    CHECK(whole.y1 == 720);

    // A square eye with the axis in the middle: width-limited, 16:9.
    const splat::TvCrop square = splat::tv_crop(512.0f, 512.0f, 1024, 1024);
    CHECK(square.width() == 1024);
    CHECK(square.height() == doctest::Approx(576).epsilon(0.002));

    // Degenerate input is an invalid crop or the image centre, never a crash or a
    // region outside the image.
    CHECK_FALSE(splat::tv_crop(0.0f, 0.0f, 0, 0).valid());
    CHECK_FALSE(splat::tv_crop(1.0f, 1.0f, 100, 100, 0, 0).valid());
    const splat::TvCrop nan_axis = splat::tv_crop(NAN, NAN, 1280, 720);
    CHECK(nan_axis.valid());
    CHECK(nan_axis.x0 >= 0);
    CHECK(nan_axis.x1 <= 1280);
    const splat::TvCrop at_edge = splat::tv_crop(0.0f, 100.0f, 400, 400);   // axis on the border
    CHECK(at_edge.valid());
    CHECK(at_edge.x1 <= 400);
}

#if defined(SPLAT_HAVE_VULKAN)

namespace {

// The overlay shader's pinhole (eye offset and shift 0): camera-space point -> pixel.
struct Px {
    double x;
    double y;
};
Px project(const splat::OverlayCamera& camera, const double p[3]) {
    return {camera.intrinsics.fx * p[0] / p[2] + camera.intrinsics.cx,
            camera.intrinsics.fy * p[1] / p[2] + camera.intrinsics.cy};
}
void corner(const splat::OverlayQuad& quad, double u, double v, double out[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = quad.center[axis] + (u - 0.5) * quad.width * quad.right[axis] +
                    (0.5 - v) * quad.height * quad.up[axis];
    }
}

}  // namespace

TEST_CASE("the TV camera puts the flat menu and a banner on exact pixels") {
    const splat::OverlayCamera camera = splat::tv_overlay_camera();
    CHECK(camera.width == splat::kTvWidth);
    CHECK(camera.height == splat::kTvHeight);
    CHECK(camera.intrinsics.valid());
    CHECK(camera.intrinsics.cx == doctest::Approx(960.0f));
    CHECK(camera.intrinsics.cy == doctest::Approx(540.0f));

    // The menu: tv_panel_quad fills the whole TV image corner to corner, for the
    // canvas the VR panel uses (1280x720).
    const splat::OverlayQuad panel = splat::tv_panel_quad(camera, 1280, 720);
    REQUIRE(panel.width > 0.0f);
    const double corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const double want[4][2] = {{0, 0}, {1920, 0}, {1920, 1080}, {0, 1080}};
    for (int i = 0; i < 4; ++i) {
        double world[3];
        corner(panel, corners[i][0], corners[i][1], world);
        const Px pixel = project(camera, world);
        CHECK(pixel.x == doctest::Approx(want[i][0]).epsilon(1e-4));
        CHECK(pixel.y == doctest::Approx(want[i][1]).epsilon(1e-4));
    }

    // A banner: a pixel rectangle comes out where it was asked for, and its top
    // edge is the top (v = 0 is the first texture row).
    const splat::OverlayQuad banner = splat::tv_screen_quad(camera, 400.0f, 900.0f, 1120.0f, 60.0f);
    const double banner_corners[4][2] = {{400, 900}, {1520, 900}, {1520, 960}, {400, 960}};
    for (int i = 0; i < 4; ++i) {
        double world[3];
        corner(banner, corners[i][0], corners[i][1], world);
        const Px pixel = project(camera, world);
        CHECK(pixel.x == doctest::Approx(banner_corners[i][0]).epsilon(1e-4));
        CHECK(pixel.y == doctest::Approx(banner_corners[i][1]).epsilon(1e-4));
    }
}

#endif  // SPLAT_HAVE_VULKAN

// ---- the bounded drop queue --------------------------------------------------

TEST_CASE("the TV slot queue never waits, is bounded and hands over the newest frame") {
    splat::TvSlotQueue queue(3);
    CHECK(queue.slots() == 3);

    // Every slot can be taken by the render loop; the next ask is refused at once
    // (a TV frame dropped, the headset loop untouched) and counted.
    const int a = queue.acquire();
    const int b = queue.acquire();
    const int c = queue.acquire();
    CHECK(a >= 0);
    CHECK(b >= 0);
    CHECK(c >= 0);
    CHECK(a != b);
    CHECK(b != c);
    CHECK(queue.acquire() == -1);
    CHECK(queue.refused() == 1);

    // Nothing published: the worker's wait times out.
    CHECK(queue.take(std::chrono::milliseconds(5)) == -1);

    // Two frames published before the worker looks: it gets the newest, the old
    // one is dropped back to free (bounded, no backlog).
    queue.publish(a);
    queue.publish(b);
    CHECK(queue.replaced() == 1);
    const int taken = queue.take(std::chrono::milliseconds(5));
    CHECK(taken == b);
    CHECK(queue.take(std::chrono::milliseconds(5)) == -1);   // a was dropped, not queued
    // a is free again, b is being read, c still belongs to the render loop.
    CHECK(queue.acquire() == a);
    CHECK(queue.acquire() == -1);
    queue.release(taken);
    CHECK(queue.acquire() == b);

    // A frame that failed gives its slot back without a publish.
    queue.cancel(c);
    CHECK(queue.acquire() == c);

    // Misuse is harmless: a double release, a publish of a slot nobody holds.
    queue.release(taken);
    queue.publish(99);
    queue.publish(-1);
    queue.cancel(99);

    // close() ends the worker's wait and stops new slots.
    std::atomic<int> woke{0};
    std::thread waiting([&] {
        woke = queue.take(std::chrono::seconds(10));
        if (woke == -1) woke = -2;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    queue.close();
    waiting.join();
    CHECK(woke == -2);
    CHECK(queue.acquire() == -1);
}

TEST_CASE("the TV slot queue passes frames between two threads in order and without a deadlock") {
    splat::TvSlotQueue queue(3);
    std::vector<int> frame_in_slot(3, 0);
    std::vector<int> seen;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        while (!done) {
            const int slot = queue.take(std::chrono::milliseconds(20));
            if (slot < 0) continue;
            seen.push_back(frame_in_slot[static_cast<size_t>(slot)]);
            // A slow TV: the render loop is faster than this.
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            queue.release(slot);
        }
    });
    int produced = 0;
    for (int frame = 1; frame <= 400; ++frame) {
        const int slot = queue.acquire();
        if (slot < 0) continue;   // dropped, never waited
        frame_in_slot[static_cast<size_t>(slot)] = frame;
        queue.publish(slot);
        ++produced;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    done = true;
    queue.close();
    worker.join();
    CHECK(produced > 50);
    CHECK_FALSE(seen.empty());
    // Strictly increasing: the worker never goes back to an older picture.
    for (size_t i = 1; i < seen.size(); ++i) CHECK(seen[i] > seen[i - 1]);
    // And nothing is stuck busy afterwards.
    queue.release(0);
}

// ---- the picture itself (GPU) ------------------------------------------------

#if defined(SPLAT_HAVE_VULKAN)

namespace {

constexpr int kEyeW = 256;
constexpr int kEyeH = 192;
constexpr float kFocal = 128.0f;
constexpr uint32_t kTvW = 160;
constexpr uint32_t kTvH = 90;

std::vector<uint8_t> solid_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    return pixels;
}

struct Rgb {
    int r, g, b;
};
Rgb tv_pixel(const uint8_t* bytes, uint32_t x, uint32_t y) {
    const uint8_t* p = bytes + (static_cast<size_t>(y) * kTvW + x) * 4;
    return {p[0], p[1], p[2]};   // R, G, B, A in memory: the target's channel order
}

}  // namespace

TEST_CASE("the TV picture has the flat menu and none of the VR window, label or HUD") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeW;
    options.eye_height = kEyeH;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {kFocal, kFocal, 0.5f * kEyeW, 0.5f * kEyeH};
    options.eye_center_shift[0] = 20.0f;
    options.eye_center_shift[1] = -20.0f;
    // A mid grey backdrop: the frame is the backdrop plus whatever overlay is
    // drawn, so every non-grey pixel of the TV picture is a TV quad and every
    // red pixel would be the VR window.
    options.backdrop[0] = options.backdrop[1] = options.backdrop[2] = 0.2f;

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, -50.0f};
    scene.log_scale = {-4.0f, -4.0f, -4.0f};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sh_dc = {0.5f, 0.5f, 0.5f};
    scene.opacity_logit = {-20.0f};
    REQUIRE(scene.consistent());

    auto made = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(made.ok(), made.message());
    splat::SplatRenderer& renderer = *made.value();

    // Before the TV is enabled a request is ignored and costs nothing.
    CHECK_FALSE(renderer.tv_capture_enabled());
    CHECK(renderer.tv_frame_pixels(0) == nullptr);
    REQUIRE_MESSAGE(renderer.enable_tv_capture(kTvW, kTvH, 2).ok(), "enable_tv_capture");
    CHECK(renderer.tv_capture_enabled());
    CHECK(renderer.tv_width() == kTvW);
    REQUIRE(renderer.tv_frame_pixels(0) != nullptr);
    REQUIRE(renderer.tv_frame_pixels(1) != nullptr);
    CHECK(renderer.tv_frame_pixels(2) == nullptr);

    const std::vector<uint8_t> red = solid_rgba(4, 4, 255, 0, 0);
    const std::vector<uint8_t> green = solid_rgba(4, 4, 0, 255, 0);
    const std::vector<uint8_t> blue = solid_rgba(4, 4, 0, 0, 255);
    auto red_texture = renderer.add_overlay_rgba_texture(4, 4, red.data());
    auto green_texture = renderer.add_overlay_rgba_texture(4, 4, green.data());
    auto blue_texture = renderer.add_overlay_rgba_texture(4, 4, blue.data());
    REQUIRE(red_texture.ok());
    REQUIRE(green_texture.ok());
    REQUIRE(blue_texture.ok());

    // The eyes' overlay list: a big red "VR window" dead ahead and a green label
    // under it - both well inside the TV's crop of the left eye.
    splat::OverlayQuad window;
    window.center[2] = 1.0f;
    window.right[0] = 1.0f;
    window.up[1] = -1.0f;
    window.width = 1.2f;
    window.height = 0.9f;
    window.texture = red_texture.value();
    splat::OverlayQuad label = window;
    label.center[1] = 0.55f;
    label.width = 0.5f;
    label.height = 0.12f;
    label.texture = green_texture.value();
    REQUIRE(renderer.set_overlay_quads({window, label}).ok());

    // The TV's list: the flat panel over the whole picture.
    splat::OverlayQuad flat = splat::tv_panel_quad(splat::tv_overlay_camera(kTvW, kTvH), 4, 4);
    flat.texture = blue_texture.value();

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, renderer.target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());
    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    splat::FrameView frame;
    frame.ipd = 0.064f;

    // --- the menu is closed: no TV quads. The TV picture is the scene alone.
    renderer.request_tv_frame(0);
    REQUIRE_MESSAGE(renderer.render(frame, target, true).ok(), "render");
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    {
        const uint8_t* tv = renderer.tv_frame_pixels(0);
        int not_backdrop = 0;
        Rgb first = tv_pixel(tv, 0, 0);
        for (uint32_t y = 0; y < kTvH; ++y) {
            for (uint32_t x = 0; x < kTvW; ++x) {
                const Rgb p = tv_pixel(tv, x, y);
                if (std::abs(p.r - first.r) > 1 || std::abs(p.g - first.g) > 1 ||
                    std::abs(p.b - first.b) > 1) {
                    ++not_backdrop;
                }
            }
        }
        // 0.2 linear is 124 in sRGB: the backdrop, and nothing but the backdrop.
        CHECK(first.r == doctest::Approx(124).epsilon(0.03));
        CHECK(first.r == first.g);
        CHECK(first.g == first.b);
        CHECK_MESSAGE(not_backdrop == 0, "pixels that are not the backdrop: " << not_backdrop);
        // The window really is in the eyes (the check is not vacuous) ...
        auto eyes = renderer.read_image();
        REQUIRE(eyes.ok());
        int red_pixels = 0;
        int green_pixels = 0;
        for (int y = 0; y < kEyeH; ++y) {
            for (int x = 0; x < kEyeW; ++x) {
                const uint8_t* p = eyes.value().pixel(x, y);
                if (p[0] > 200 && p[1] < 60) ++red_pixels;
                if (p[1] > 200 && p[0] < 60) ++green_pixels;
            }
        }
        CHECK(red_pixels > 3000);
        CHECK(green_pixels > 300);
    }

    // --- the menu is open: the flat panel fills the TV picture, the window still
    // is not in it, and the eyes are unchanged.
    REQUIRE(renderer.set_tv_overlay_quads({flat}).ok());
    renderer.request_tv_frame(1);
    REQUIRE_MESSAGE(renderer.render(frame, target, true).ok(), "render with the menu");
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    {
        const uint8_t* tv = renderer.tv_frame_pixels(1);
        int blue_pixels = 0;
        int stray = 0;
        for (uint32_t y = 0; y < kTvH; ++y) {
            for (uint32_t x = 0; x < kTvW; ++x) {
                const Rgb p = tv_pixel(tv, x, y);
                if (p.b > 250 && p.r < 5 && p.g < 5) ++blue_pixels;
                if (p.r > 40 && p.g < 5 && p.b < 5) ++stray;     // the window's red
                if (p.g > 40 && p.r < 5 && p.b < 5) ++stray;     // the label's green
            }
        }
        CHECK(blue_pixels == static_cast<int>(kTvW * kTvH));   // corner to corner
        CHECK(stray == 0);
        auto eyes = renderer.read_image();
        REQUIRE(eyes.ok());
        int red_pixels = 0;
        int blue_in_eyes = 0;
        for (int y = 0; y < kEyeH; ++y) {
            for (int x = 0; x < kEyeW; ++x) {
                const uint8_t* p = eyes.value().pixel(x, y);
                if (p[0] > 200 && p[1] < 60) ++red_pixels;
                if (p[2] > 200 && p[0] < 60 && p[1] < 60) ++blue_in_eyes;
            }
        }
        CHECK(red_pixels > 3000);       // the VR window is still in the eyes
        CHECK(blue_in_eyes == 0);       // and the TV's flat panel is not
    }

    // --- a half-width TV quad (the waiting banner's shape): only its pixels change.
    splat::OverlayQuad half = splat::tv_screen_quad(splat::tv_overlay_camera(kTvW, kTvH), 0.0f,
                                                    0.0f, 0.5f * kTvW, static_cast<float>(kTvH));
    half.texture = blue_texture.value();
    REQUIRE(renderer.set_tv_overlay_quads({half}).ok());
    renderer.request_tv_frame(0);
    REQUIRE(renderer.render(frame, target, true).ok());
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    {
        const uint8_t* tv = renderer.tv_frame_pixels(0);
        CHECK(tv_pixel(tv, 10, 45).b > 250);
        CHECK(tv_pixel(tv, 10, 45).r < 5);
        const Rgb right = tv_pixel(tv, kTvW - 10, 45);
        CHECK(right.b < 200);        // the scene side: backdrop grey
        CHECK(right.r == right.b);
    }

    // --- a frame that asks for no TV picture leaves its slot exactly as it was.
    const uint8_t* slot = renderer.tv_frame_pixels(1);
    const std::vector<uint8_t> before(slot, slot + static_cast<size_t>(kTvW) * kTvH * 4);
    renderer.request_tv_frame(-1);
    REQUIRE(renderer.render(frame, target, true).ok());
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    CHECK(std::equal(before.begin(), before.end(), renderer.tv_frame_pixels(1)));
    // And a request is for one frame only: it was consumed by the render above.
    REQUIRE(renderer.set_tv_overlay_quads({flat}).ok());
    renderer.request_tv_frame(0);
    REQUIRE(renderer.render(frame, target, true).ok());
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const uint8_t* slot0 = renderer.tv_frame_pixels(0);
    const std::vector<uint8_t> flat_frame(slot0, slot0 + before.size());
    REQUIRE(renderer.set_tv_overlay_quads({half}).ok());
    REQUIRE(renderer.render(frame, target, true).ok());   // no request this time
    target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    CHECK(std::equal(flat_frame.begin(), flat_frame.end(), renderer.tv_frame_pixels(0)));

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("the TV picture follows the gamma-space blend target too") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeW;
    options.eye_height = kEyeH;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {kFocal, kFocal, 0.5f * kEyeW, 0.5f * kEyeH};
    options.backdrop[0] = options.backdrop[1] = options.backdrop[2] = 0.0f;

    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, -50.0f};
    scene.log_scale = {-4.0f, -4.0f, -4.0f};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sh_dc = {0.5f, 0.5f, 0.5f};
    scene.opacity_logit = {-20.0f};
    auto made = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(made.ok(), made.message());
    splat::SplatRenderer& renderer = *made.value();
    REQUIRE(renderer.enable_tv_capture(kTvW, kTvH, 1).ok());

    const std::vector<uint8_t> blue = solid_rgba(4, 4, 0, 0, 255);
    auto blue_texture = renderer.add_overlay_rgba_texture(4, 4, blue.data());
    REQUIRE(blue_texture.ok());
    splat::OverlayQuad flat = splat::tv_panel_quad(splat::tv_overlay_camera(kTvW, kTvH), 4, 4);
    flat.texture = blue_texture.value();
    REQUIRE(renderer.set_tv_overlay_quads({flat}).ok());

    // The UNORM counterpart is what Display > Blending > gamma-space renders to.
    const VkFormat alt = splat::blend_alt_format(options.target_format);
    REQUIRE(alt != options.target_format);
    auto image = splat::create_vulkan_color_image(
        device, alt, renderer.target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());
    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = renderer.target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    target.format = alt;

    splat::FrameView frame;
    renderer.request_tv_frame(0);
    REQUIRE_MESSAGE(renderer.render(frame, target, true).ok(), "render to the UNORM target");
    const uint8_t* tv = renderer.tv_frame_pixels(0);
    // The same bytes either way: blue stays blue (an encoded value, not a darker
    // one that a missed encode would give).
    int blue_pixels = 0;
    for (uint32_t y = 0; y < kTvH; ++y) {
        for (uint32_t x = 0; x < kTvW; ++x) {
            const Rgb p = tv_pixel(tv, x, y);
            if (p.b > 250 && p.r < 5 && p.g < 5) ++blue_pixels;
        }
    }
    CHECK(blue_pixels == static_cast<int>(kTvW * kTvH));
    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("the TV picture render test is skipped without Vulkan") { CHECK(true); }

#endif  // SPLAT_HAVE_VULKAN
