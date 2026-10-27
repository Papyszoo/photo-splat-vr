// VR9: the overlay textures get a full mip chain, so the menu panel and the
// scene label are sampled trilinearly (with anisotropy when the device has it)
// instead of minifying level 0 and flickering (headset test 6).
//
// The level count is a pure function; the contents are checked the way the
// headset sees them: a 1-texel checkerboard minified to a 16 px quad must come
// out an even grey, not a black/white pattern.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "render/vulkan_context.h"

#if defined(SPLAT_HAVE_VULKAN)

TEST_CASE("vr9 the mip chain covers every halving down to one texel") {
    CHECK(splat::mip_level_count(1, 1) == 1);
    CHECK(splat::mip_level_count(2, 1) == 2);
    CHECK(splat::mip_level_count(3, 1) == 2);
    CHECK(splat::mip_level_count(64, 64) == 7);
    CHECK(splat::mip_level_count(896, 504) == 10);
    CHECK(splat::mip_level_count(1280, 720) == 11);
    // The longest side decides: a 1-px-high strip still halves with the width.
    CHECK(splat::mip_level_count(8, 1) == 4);
}

#include "render/splat_renderer.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

double luminance(const uint8_t* pixel) {
    return (static_cast<double>(pixel[0]) + pixel[1] + pixel[2]) / 3.0;
}

}  // namespace

TEST_CASE("vr9 a minified overlay texture samples the average, not level 0") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();
    MESSAGE("samplerAnisotropy " << (device.sampler_anisotropy() ? "yes" : "no") << ", max "
                                 << device.max_sampler_anisotropy());

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 64;
    options.eye_height = 64;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {70.0f, 70.0f, 32.0f, 32.0f};
    options.backdrop[0] = options.backdrop[1] = options.backdrop[2] = 0.0f;

    // One splat behind the camera: nothing but the overlay is visible.
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, -1.0f};
    scene.log_scale = {-4.0f, -4.0f, -4.0f};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sh_dc = {0.5f, 0.5f, 0.5f};
    scene.opacity_logit = {0.0f};
    REQUIRE(scene.consistent());

    auto renderer = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());

    // A 64x64 checkerboard of single texels: 0 or 255 in every channel. Its mip
    // levels are exact averages (level 2 is a uniform mid-grey).
    constexpr uint32_t kSize = 64;
    std::vector<uint8_t> checker(static_cast<size_t>(kSize) * kSize * 4);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const uint8_t value = ((x + y) & 1u) != 0 ? 255 : 0;
            uint8_t* pixel = checker.data() + (static_cast<size_t>(y) * kSize + x) * 4;
            pixel[0] = pixel[1] = pixel[2] = value;
            pixel[3] = 255;
        }
    }
    auto texture = renderer.value()->add_overlay_rgba_texture(kSize, kSize, checker.data());
    REQUIRE_MESSAGE(texture.ok(), texture.message());

    // At z = 1 and fx = 70, a width of 16/70 projects to 16 px: each output
    // pixel covers 4 texels, so a correct mip chain samples level 2 (4x4
    // averages) and the result is uniform.
    splat::OverlayQuad quad;
    quad.center[2] = 1.0f;
    quad.width = 16.0f / 70.0f;
    quad.height = 16.0f / 70.0f;
    quad.texture = texture.value();
    REQUIRE(renderer.value()->set_overlay_quads({quad}).ok());

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
    REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, target, true).ok(), "render");
    auto pixels = renderer.value()->read_image();
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());

    // The quad covers x, y = 24 .. 40; the inner 8x8 pixels are all texture.
    double sum = 0.0;
    double low = 1.0e9;
    double high = -1.0e9;
    int count = 0;
    for (int y = 28; y < 36; ++y) {
        for (int x = 28; x < 36; ++x) {
            const double value = luminance(pixels.value().pixel(x, y));
            sum += value;
            low = std::min(low, value);
            high = std::max(high, value);
            ++count;
        }
    }
    const double mean = sum / static_cast<double>(count);
    MESSAGE("minified checker: mean " << mean << ", spread " << (high - low));
    // Level 0 would alternate near 0/255. A mip-filtered sample of a half-black
    // half-white checker is a smooth mid tone (sRGB-encoded: ~188; some drivers
    // filter in encoded space and give ~128).
    CHECK(low > 70.0);
    CHECK(high < 245.0);
    CHECK(mean > 100.0);
    CHECK(mean < 230.0);
    CHECK(high - low < 60.0);

    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("vr9 the overlay mip test is skipped without Vulkan") { CHECK(true); }

#endif
