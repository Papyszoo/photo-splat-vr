// Desktop check of the RGBA8 overlay texture, the path the UI panel uses
// (docs/ui-v1-plan.md: the CPU-drawn canvas uploaded as ONE texture).
//
// A photo-like canvas (a smooth sky/ground ramp with the per-pixel grain a
// photo has, drawn through UiCanvas::blit like a thumbnail) is uploaded with
// add_overlay_rgba_texture and projected over the whole view with a TV panel
// quad. The frame that comes back must match the canvas within 1 LSB per
// channel: the sRGB texture decode and the sRGB target encode are the only
// steps, and they round-trip. The same slot is then updated in place with a
// second canvas (update_overlay_rgba_texture, the once-per-change upload) and
// checked again. Finally an R8 mask quad is drawn on the same renderer to show
// the text/label path still works next to the panel pipeline.
//
// Needs a Vulkan device; without one the case skips with a message.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/image.h"
#include "render/overlay.h"
#include "render/splat_renderer.h"
#include "render/ui_panel.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"
#include "ui/canvas.h"
#include "ui/image.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

constexpr uint32_t kWidth = 128;
constexpr uint32_t kHeight = 72;

uint8_t clamp_byte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// A photo-like RGBA image: a bright sky fading into a warm ground, one soft
// highlight, and +-32 levels of deterministic grain on every pixel. A 48-colour
// palette could not reproduce it, which is the point of the RGBA texture.
splat::UiImage photo_like(uint32_t width, uint32_t height, uint32_t seed) {
    splat::UiImage image;
    image.resize(width, height);
    uint32_t state = seed * 1664525u + 1013904223u;
    for (uint32_t y = 0; y < height; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(height - 1);
        for (uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width - 1);
            state = state * 1664525u + 1013904223u;
            const int grain = static_cast<int>((state >> 24) & 0x3Fu) - 32;
            const float highlight =
                60.0f * std::exp(-40.0f * ((u - 0.3f) * (u - 0.3f) + (v - 0.25f) * (v - 0.25f)));
            uint8_t* p = image.pixel(x, y);
            p[0] = clamp_byte(static_cast<int>(30.0f + 120.0f * u + 90.0f * v + highlight) + grain);
            p[1] = clamp_byte(static_cast<int>(60.0f + 80.0f * (1.0f - v) + 40.0f * u) + grain);
            p[2] = clamp_byte(static_cast<int>(210.0f - 150.0f * v + 25.0f * u) - grain);
            p[3] = 255;
        }
    }
    return image;
}

// Largest per-channel difference between the canvas bytes and the read-back
// frame (both RGB in sRGB encoding).
int max_channel_delta(const std::vector<uint8_t>& rgba, const splat::Image& frame) {
    int worst = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const uint8_t* want =
                rgba.data() + (static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4;
            const uint8_t* got = frame.pixel(x, y);
            for (int channel = 0; channel < 3; ++channel) {
                worst = std::max(worst, std::abs(static_cast<int>(want[channel]) -
                                                 static_cast<int>(got[channel])));
            }
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("overlay RGBA panel texture keeps a photo-like canvas within 1 LSB") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kWidth;
    options.eye_height = kHeight;
    // The TV target: B8G8R8A8_SRGB, read back as BGRA (SplatRenderer::read_image
    // swaps to RGB), the same format main.cpp's shots come from.
    options.target_format = VK_FORMAT_B8G8R8A8_SRGB;
    options.intrinsics = {0.5f * kWidth / std::tan(30.0f * 3.14159265f / 180.0f),
                          0.5f * kWidth / std::tan(30.0f * 3.14159265f / 180.0f),
                          0.5f * kWidth, 0.5f * kHeight};

    // One small splat behind the panel: the panel is opaque and has no depth
    // test, so it covers every pixel and the splat only satisfies create().
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, 1.0f};
    scene.log_scale = {std::log(0.01f), std::log(0.01f), std::log(0.01f)};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sh_dc = {0.0f, 0.0f, 0.0f};
    scene.opacity_logit = {12.0f};

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

    auto render = [&]() {
        const splat::Status status = renderer.value()->render(splat::FrameView{}, target, true);
        REQUIRE_MESSAGE(status.ok(), status.message());
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer.value()->read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        return pixels.take();
    };

    // The overlay camera: the identity view (zero pose) and the renderer's own
    // lens, so tv_panel_quad() fills the viewport corner to corner.
    splat::OverlayCamera camera;
    camera.view[0] = 1.0f;
    camera.view[5] = 1.0f;
    camera.view[10] = 1.0f;
    camera.view[15] = 1.0f;
    camera.intrinsics = options.intrinsics;
    camera.width = kWidth;
    camera.height = kHeight;

    // The canvas is drawn the way a screen draws a photo: a full-canvas blit of
    // an RGBA image. Opaque, so the read-back is the canvas colour and nothing
    // else.
    splat::UiCanvas canvas(kWidth, kHeight);
    canvas.blit(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight),
                photo_like(kWidth, kHeight, 1), false);
    REQUIRE(canvas.pixels().size() == static_cast<size_t>(kWidth) * kHeight * 4);

    auto texture = renderer.value()->add_overlay_rgba_texture(
        kWidth, kHeight, canvas.pixels().data());
    REQUIRE_MESSAGE(texture.ok(), texture.message());

    splat::OverlayQuad quad = splat::tv_panel_quad(camera, kWidth, kHeight);
    quad.texture = texture.value();
    REQUIRE_MESSAGE(renderer.value()->set_overlay_quads({quad}).ok(), "set_overlay_quads");

    const splat::Image first = render();
    const int first_delta = max_channel_delta(canvas.pixels(), first);
    MESSAGE("panel RGBA upload: max channel difference " << first_delta << " LSB");
    CHECK(first_delta <= 1);

    // The once-per-change upload path: the same texture slot, a new canvas.
    splat::UiCanvas second_canvas(kWidth, kHeight);
    second_canvas.blit(0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight),
                       photo_like(kWidth, kHeight, 7), false);
    const splat::Status updated = renderer.value()->update_overlay_rgba_texture(
        texture.value(), kWidth, kHeight, second_canvas.pixels().data());
    REQUIRE_MESSAGE(updated.ok(), updated.message());
    CHECK(renderer.value()->has_overlay_quads());

    const splat::Image second = render();
    const int second_delta = max_channel_delta(second_canvas.pixels(), second);
    MESSAGE("panel RGBA update: max channel difference " << second_delta << " LSB");
    CHECK(second_delta <= 1);

    // The text/label path is unchanged: a 1x1 white R8 mask with a red tint
    // fills the view with exactly red, on the same overlay renderer.
    const uint8_t white = 255;
    auto mask = renderer.value()->add_overlay_texture(1, 1, &white);
    REQUIRE_MESSAGE(mask.ok(), mask.message());
    splat::OverlayQuad tinted = splat::tv_panel_quad(camera, kWidth, kHeight);
    tinted.texture = mask.value();
    tinted.tint[0] = 1.0f;
    tinted.tint[1] = 0.0f;
    tinted.tint[2] = 0.0f;
    tinted.tint[3] = 1.0f;
    REQUIRE_MESSAGE(renderer.value()->set_overlay_quads({tinted}).ok(), "set_overlay_quads");
    const splat::Image masked = render();
    const uint8_t* red = masked.pixel(kWidth / 2, kHeight / 2);
    CHECK(static_cast<int>(red[0]) >= 254);
    CHECK(static_cast<int>(red[1]) <= 1);
    CHECK(static_cast<int>(red[2]) <= 1);

    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("overlay RGBA panel texture is skipped without Vulkan") {
    MESSAGE("skipping: built without Vulkan, so there is no overlay renderer to upload to");
    CHECK(true);
}

#endif  // SPLAT_HAVE_VULKAN
