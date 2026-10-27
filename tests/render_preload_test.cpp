#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <cmath>
#include <cstdint>

#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

splat::SplatScene make_scene(size_t count) {
    splat::SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        // Spread along x so every splat projects inside the small viewport.
        scene.position[3 * i + 0] = (static_cast<float>(i) - 0.5f * static_cast<float>(count)) * 0.1f;
        scene.position[3 * i + 1] = 0.0f;
        scene.position[3 * i + 2] = 1.0f;
        scene.log_scale[3 * i + 0] = std::log(0.02f);
        scene.log_scale[3 * i + 1] = std::log(0.02f);
        scene.log_scale[3 * i + 2] = std::log(0.02f);
        scene.rotation[4 * i + 0] = 1.0f;
        const float white_dc = 0.5f / 0.28209479177387814f;
        scene.sh_dc[3 * i + 0] = white_dc;
        scene.sh_dc[3 * i + 1] = white_dc;
        scene.sh_dc[3 * i + 2] = white_dc;
        scene.opacity_logit[i] = 12.0f;
    }
    return scene;
}

}  // namespace

TEST_CASE("preloaded scene keeps the old buffers valid until the swap") {
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
    options.intrinsics = {100.0f, 100.0f, 64.0f, 48.0f};

    const splat::SplatScene first = make_scene(1);
    auto renderer = splat::SplatRenderer::create(device, options, first);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());
    splat::SplatRenderer& splat_renderer = *renderer.value();

    auto image = splat::create_vulkan_color_image(
        device, options.target_format, splat_renderer.target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(image.ok(), image.message());

    splat::RenderTarget target;
    target.image = image.value().image;
    target.view = image.value().view;
    target.extent = splat_renderer.target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    auto render_frame = [&]() {
        const splat::Status status = splat_renderer.render(splat::FrameView{}, target, false);
        REQUIRE_MESSAGE(status.ok(), status.message());
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        return splat_renderer.last_visible_count();
    };

    CHECK(render_frame() == 1);
    CHECK(splat_renderer.scene_count() == 1);
    CHECK(splat_renderer.scene_generation() == 0);
    CHECK_FALSE(splat_renderer.has_staged_scene());

    const splat::SplatScene second = make_scene(3);
    const splat::Status staged = splat_renderer.stage_scene(second);
    REQUIRE_MESSAGE(staged.ok(), staged.message());
    CHECK(splat_renderer.has_staged_scene());
    CHECK(splat_renderer.staged_count() == 3);
    // At least the five attribute buffers (3 splats * 14 fp16 values); when
    // the staged scene is larger than the active capacity it also owns its
    // instance/key/value arrays.
    CHECK(splat_renderer.staged_bytes() >= 3u * 14u * 2u);
    CHECK(splat_renderer.scene_generation() == 0);

    // While staged, the old scene is still the one the renderer draws and its
    // buffers stay valid.
    CHECK(render_frame() == 1);
    CHECK(splat_renderer.scene_count() == 1);

    const splat::Status committed = splat_renderer.commit_staged_scene();
    REQUIRE_MESSAGE(committed.ok(), committed.message());
    CHECK_FALSE(splat_renderer.has_staged_scene());
    CHECK(splat_renderer.scene_count() == 3);
    CHECK(splat_renderer.scene_generation() == 1);
    CHECK(splat_renderer.last_commit_ms() >= 0.0);
    CHECK(render_frame() == 3);

    // A larger staged scene also stages its own instance/key/value buffers;
    // staged_bytes reports the whole preload footprint.
    const splat::SplatScene larger = make_scene(8);
    REQUIRE(splat_renderer.stage_scene(larger).ok());
    CHECK(splat_renderer.staged_bytes() > 8u * 14u * 2u);
    REQUIRE(splat_renderer.commit_staged_scene().ok());
    CHECK(splat_renderer.scene_count() == 8);
    CHECK(render_frame() == 8);

    // Discarding a staged scene leaves the committed one untouched.
    REQUIRE(splat_renderer.stage_scene(first).ok());
    splat_renderer.discard_staged_scene();
    CHECK_FALSE(splat_renderer.has_staged_scene());
    CHECK(splat_renderer.scene_count() == 8);
    CHECK(render_frame() == 8);

    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("scene preload swap is skipped without Vulkan") { CHECK(true); }

#endif
