// VR8: the cost of the cheap Display toggles, measured on the desktop GPU.
//
// eps2d only changes the projected covariance diagonal (a few FMAs per splat)
// and the 3-sigma quad width a little, so its cost should be inside the noise;
// the blending toggle is a pipeline format change only. This test renders the
// real SHARP scene at 960x960 and prints the timestamped project/sort/draw
// times for 0.3 vs 0.5 and for the sRGB vs UNORM target. It skips when the
// scene or a Vulkan device is absent (AGENTS.md); the numbers are for the
// report, not a pass/fail threshold.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <filesystem>
#include <memory>

#include "data_dir.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/scene_folder.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

struct Timing {
    double project = 0.0;
    double sort = 0.0;
    double draw = 0.0;
};

Timing render_average(splat::SplatRenderer& renderer, splat::VulkanContext& device,
                      splat::RenderTarget& target, int frames) {
    Timing timing;
    for (int i = 0; i < frames; ++i) {
        REQUIRE_MESSAGE(renderer.render(splat::FrameView{}, target, false).ok(), "render");
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        timing.project += renderer.timings().project_ms;
        timing.sort += renderer.timings().sort_ms;
        timing.draw += renderer.timings().draw_ms;
    }
    const double count = static_cast<double>(frames);
    timing.project /= count;
    timing.sort /= count;
    timing.draw /= count;
    (void)device;
    return timing;
}

}  // namespace

TEST_CASE("render AA and blend toggles cost on a desktop GPU") {
    const std::filesystem::path folder = splat_test::data_path("runs/sharp_e2e/immichvr-1");
    if (!std::filesystem::exists(folder / "scene.psplat")) {
        MESSAGE("skipping: " << folder << " is absent (external data)");
        return;
    }
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();
    auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 960;
    options.eye_height = 960;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {700.0f, 700.0f, 480.0f, 480.0f};

    auto renderer = splat::SplatRenderer::create(device, options, loaded.value().scene);
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

    constexpr int kFrames = 8;
    auto warm_up = [&](splat::RenderTarget& image_target) {
        REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, image_target, false).ok(),
                        "warm-up render");
        image_target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    };
    renderer.value()->set_low_pass_eps(0.3f);
    warm_up(target);
    const Timing standard = render_average(*renderer.value(), device, target, kFrames);
    renderer.value()->set_low_pass_eps(0.5f);
    warm_up(target);
    const Timing soft = render_average(*renderer.value(), device, target, kFrames);
    MESSAGE("eps2d 0.3: project " << standard.project << " sort " << standard.sort << " draw "
                                  << standard.draw << " ms");
    MESSAGE("eps2d 0.5: project " << soft.project << " sort " << soft.sort << " draw "
                                  << soft.draw << " ms");

    // The same frame through the UNORM counterpart (gamma-space blending). No
    // readback, so only the pipeline differs.
    const VkFormat gamma_format = splat::blend_alt_format(options.target_format);
    if (gamma_format != options.target_format) {
        auto gamma_image = splat::create_vulkan_color_image(
            device, gamma_format, renderer.value()->target_extent(),
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        REQUIRE_MESSAGE(gamma_image.ok(), gamma_image.message());
        splat::RenderTarget gamma = target;
        gamma.image = gamma_image.value().image;
        gamma.view = gamma_image.value().view;
        gamma.format = gamma_format;
        gamma.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        renderer.value()->set_low_pass_eps(0.3f);
        warm_up(target);
        const Timing linear = render_average(*renderer.value(), device, target, kFrames);
        warm_up(gamma);
        const Timing srgb = render_average(*renderer.value(), device, gamma, kFrames);
        MESSAGE("blend Linear: project " << linear.project << " sort " << linear.sort << " draw "
                                         << linear.draw << " ms");
        MESSAGE("blend sRGB:   project " << srgb.project << " sort " << srgb.sort << " draw "
                                         << srgb.draw << " ms");
        splat::destroy_vulkan_image(device, gamma_image.value());
    }
    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("render AA and blend cost is skipped without Vulkan") { CHECK(true); }

#endif
