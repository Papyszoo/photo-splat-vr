#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <cmath>
#include <cstdint>
#include <vector>

#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

// Every splat sits at the same position and depth, so they all share one depth
// key and overlap on screen with half opacity: the blended result depends on
// the order of the tied keys. Before the deterministic compaction fix the
// order came from an atomicAdd and differed between frames.
splat::SplatScene tied_scene(size_t count) {
    splat::SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        scene.position[3 * i + 0] = 0.0f;
        scene.position[3 * i + 1] = 0.0f;
        scene.position[3 * i + 2] = 1.0f;
        const float sigma = 0.05f;
        scene.log_scale[3 * i + 0] = std::log(sigma);
        scene.log_scale[3 * i + 1] = std::log(sigma);
        scene.log_scale[3 * i + 2] = std::log(sigma);
        scene.rotation[4 * i + 0] = 1.0f;
        // A deterministic colour ramp, sRGB-encoded like a .ply SH DC.
        const float t = static_cast<float>(i) / static_cast<float>(count);
        scene.sh_dc[3 * i + 0] = t;
        scene.sh_dc[3 * i + 1] = 1.0f - t;
        scene.sh_dc[3 * i + 2] = 0.5f;
        scene.opacity_logit[i] = 0.0f;  // alpha 0.5, so order matters
    }
    return scene;
}

}  // namespace

TEST_CASE("the same pose renders byte-identical frames") {
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
    options.eye_width = 96;
    options.eye_height = 96;
    options.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    options.intrinsics = {70.0f, 70.0f, 48.0f, 48.0f};

    const splat::SplatScene scene = tied_scene(512);

    auto render_frames = [&](splat::SplatRenderer& renderer, int frame_count) {
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

        std::vector<std::vector<uint8_t>> frames;
        for (int i = 0; i < frame_count; ++i) {
            const splat::Status status = renderer.render(splat::FrameView{}, target, true);
            REQUIRE_MESSAGE(status.ok(), status.message());
            target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            auto pixels = renderer.read_image();
            REQUIRE_MESSAGE(pixels.ok(), pixels.message());
            frames.push_back(pixels.value().rgb);
        }
        splat::destroy_vulkan_image(device, image.value());
        return frames;
    };

    auto renderer = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());

    // Four frames from one renderer: no frame may differ from the first.
    const std::vector<std::vector<uint8_t>> frames = render_frames(*renderer.value(), 4);
    REQUIRE(frames.size() == 4);
    for (size_t i = 1; i < frames.size(); ++i) {
        CAPTURE(i);
        REQUIRE(frames[i].size() == frames[0].size());
        CHECK_MESSAGE(frames[i] == frames[0], "frame " << i << " matches frame 0");
    }

    // A freshly created renderer over the same scene must agree too: the
    // scene upload and the compaction order are part of the guarantee.
    auto second = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(second.ok(), second.message());
    const std::vector<std::vector<uint8_t>> other = render_frames(*second.value(), 1);
    REQUIRE(other.size() == 1);
    REQUIRE(other[0].size() == frames[0].size());
    CHECK_MESSAGE(other[0] == frames[0], "a second renderer matches the first frame");
}

#else

TEST_CASE("frame determinism is skipped without Vulkan") { CHECK(true); }

#endif
