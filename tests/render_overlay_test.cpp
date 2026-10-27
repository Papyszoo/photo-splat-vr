// Desktop check of the scene label overlay: render the real SHARP scene
// (runs/sharp_e2e/immichvr-1, external data) once without and once with the
// label, then assert that
//
//   (a) the label lands below the photo's bottom edge (the capture frustum's
//       bottom edge at the reading distance, projected),
//   (b) it really is light text on a dark translucent plate,
//   (c) every pixel outside the label's rectangle is unchanged: PSNR >= 80 dB
//       against the no-label frame, i.e. the splat pass is untouched.
//
// The frame is 960x960 with half of the capture intrinsics: the photo fills
// y = 0 .. 720, leaving the lower 240 px for the world-locked label, which is
// the region a VR eye sees below the photo. PNGs go to
// $SPLAT_DATA_DIR/runs/vr4_label/. The test skips when the scene is absent
// (AGENTS.md).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <stb/stb_image_write.h>

#include "common/image.h"
#include "data_dir.h"
#include "render/scene_label.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/scene_folder.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

namespace {

struct Rect {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

// Projects the four corners of a quad with the renderer's pinhole model, in
// one eye (the desktop test is mono, so the eye offset is zero).
Rect project_quad(const splat::OverlayQuad& quad, const splat::CameraIntrinsics& intrinsics) {
    Rect box{1.0e9, 1.0e9, -1.0e9, -1.0e9};
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            const double x = quad.center[0] + 0.5 * sx * quad.width * quad.right[0] +
                             0.5 * sy * quad.height * quad.up[0];
            const double y = quad.center[1] + 0.5 * sx * quad.width * quad.right[1] +
                             0.5 * sy * quad.height * quad.up[1];
            const double z = quad.center[2];
            const double px = intrinsics.fx * x / z + intrinsics.cx;
            const double py = intrinsics.fy * y / z + intrinsics.cy;
            box.x0 = std::min(box.x0, px);
            box.y0 = std::min(box.y0, py);
            box.x1 = std::max(box.x1, px);
            box.y1 = std::max(box.y1, py);
        }
    }
    return box;
}

double luminance(const uint8_t* pixel) {
    return (static_cast<double>(pixel[0]) + pixel[1] + pixel[2]) / 3.0;
}

bool write_png(const std::filesystem::path& path, const splat::Image& image) {
    if (image.empty()) return false;
    return stbi_write_png(path.string().c_str(), image.width, image.height, 3,
                          image.rgb.data(), image.width * 3) != 0;
}

}  // namespace

TEST_CASE("the scene label is drawn below the photo and changes nothing else") {
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
    const splat::LoadedScene& scene = loaded.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = 960;
    options.eye_height = 960;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    // Half the capture intrinsics: the 1920x1440 photo covers y = 0 .. 720 of
    // the 960 px frame, so the label's region below it is on screen.
    const splat::Intrinsics& capture = scene.meta.intrinsics;
    REQUIRE(capture.fx > 0.0f);
    const float scale = 0.5f;
    options.intrinsics = {capture.fx * scale, capture.fy * scale, capture.cx * scale,
                          capture.cy * scale};
    options.capture_intrinsics = {capture.fx, capture.fy, capture.cx, capture.cy};
    options.capture_width = capture.width;
    options.capture_height = capture.height;

    auto renderer = splat::SplatRenderer::create(device, options, scene.scene);
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
        REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, target, true).ok(), "render");
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer.value()->read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        return pixels.take();
    };

    const splat::Image plain = render();

    // The label for scene 1 of 5 with the real meta.json (backend "sharp").
    const std::string folder_name = folder.filename().string();
    auto label_result = splat::make_scene_label(scene.meta, folder_name, 0, 5);
    REQUIRE_MESSAGE(label_result.ok(), label_result.message());
    splat::SceneLabel label = label_result.take();
    CHECK(label.title == folder_name + " \xC2\xB7 SHARP");
    CHECK(label.counter == "scene 1/5");

    const uint8_t white = 255;
    auto plate_texture = renderer.value()->add_overlay_texture(1, 1, &white);
    REQUIRE_MESSAGE(plate_texture.ok(), plate_texture.message());
    auto text_texture = renderer.value()->add_overlay_texture(
        label.image.width, label.image.height, label.image.pixels.data());
    REQUIRE_MESSAGE(text_texture.ok(), text_texture.message());
    label.plate.texture = plate_texture.value();
    label.text.texture = text_texture.value();
    // Plate first, text over it.
    const splat::Status quads =
        renderer.value()->set_overlay_quads({label.plate, label.text});
    REQUIRE_MESSAGE(quads.ok(), quads.message());
    REQUIRE(renderer.value()->has_overlay_quads());

    const splat::Image labelled = render();

    // (a) Geometry: the plate is below the photo's bottom edge in pixels.
    const splat::CameraIntrinsics& lens = options.intrinsics;
    const Rect box = project_quad(label.plate, lens);
    const double photo_bottom = lens.fy * label.photo_bottom_y / label.distance + lens.cy;
    MESSAGE("label rect " << box.x0 << "," << box.y0 << " .. " << box.x1 << "," << box.y1
                          << " px; photo bottom " << photo_bottom << " px");
    CHECK(photo_bottom > 600.0);
    CHECK(box.y0 > photo_bottom - 1.0);
    CHECK(box.y1 < 960.0);
    CHECK(box.x0 > 0.0);
    CHECK(box.x1 < 960.0);

    // (b) Label pixels: a dark translucent plate changes most of the rect, and
    // the light text leaves many bright pixels.
    const int x0 = std::max(0, static_cast<int>(std::floor(box.x0)));
    const int y0 = std::max(0, static_cast<int>(std::floor(box.y0)));
    const int x1 = std::min(labelled.width, static_cast<int>(std::ceil(box.x1)));
    const int y1 = std::min(labelled.height, static_cast<int>(std::ceil(box.y1)));
    size_t inside = 0;
    size_t changed = 0;
    size_t darker = 0;
    size_t bright = 0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const double before = luminance(plain.pixel(x, y));
            const double after = luminance(labelled.pixel(x, y));
            ++inside;
            if (std::abs(after - before) > 8.0) ++changed;
            if (after < before - 20.0) ++darker;
            if (after > 200.0) ++bright;
        }
    }
    MESSAGE("label rect: " << inside << " px, " << changed << " changed, " << darker
                           << " darkened, " << bright << " bright text px");
    REQUIRE(inside > 0);
    CHECK(changed > inside * 3 / 10);  // the plate covers the whole rect
    CHECK(darker > inside * 2 / 10);   // light text on a DARK plate
    CHECK(bright > 20);                // glyph strokes near white

    // (c) Everything outside the label rect is bit-identical to the no-label
    // frame: the overlay pass draws after the splats and touches nothing else.
    double squared = 0.0;
    size_t outside = 0;
    for (int y = 0; y < labelled.height; ++y) {
        for (int x = 0; x < labelled.width; ++x) {
            if (x >= x0 && x <= x1 && y >= y0 && y <= y1) continue;
            const uint8_t* a = plain.pixel(x, y);
            const uint8_t* b = labelled.pixel(x, y);
            for (int channel = 0; channel < 3; ++channel) {
                const double difference = static_cast<double>(a[channel]) - b[channel];
                squared += difference * difference;
            }
            ++outside;
        }
    }
    REQUIRE(outside > 0);
    const double mse = squared / (static_cast<double>(outside) * 3.0);
    const double psnr = squared <= 0.0 ? 1.0e9 : 10.0 * std::log10((255.0 * 255.0) / mse);
    MESSAGE("PSNR outside the label rect: " << psnr << " dB over " << outside << " px");
    CHECK(psnr >= 80.0);

    // Review artefacts.
    const std::filesystem::path out = splat_test::data_path("runs/vr4_label");
    std::error_code error;
    std::filesystem::create_directories(out, error);
    const bool wrote_plain = write_png(out / "no_label_960x960.png", plain);
    const bool wrote_label = write_png(out / "label_960x960.png", labelled);
    CHECK(wrote_plain);
    CHECK(wrote_label);
    MESSAGE("wrote " << (out / "label_960x960.png") << " and " << (out / "no_label_960x960.png"));

    splat::destroy_vulkan_image(device, image.value());
}

TEST_CASE("the gamma-space target renders through the alternate pipelines") {
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
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {70.0f, 70.0f, 32.0f, 32.0f};
    options.backdrop[0] = options.backdrop[1] = options.backdrop[2] = 0.0f;

    // One splat behind the camera: nothing but the overlay is visible, so the
    // frame is a clean test of the blend pipeline.
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
    const uint8_t white = 255;
    auto texture = renderer.value()->add_overlay_texture(1, 1, &white);
    REQUIRE_MESSAGE(texture.ok(), texture.message());

    splat::OverlayQuad quad;
    quad.center[2] = 1.0f;
    quad.width = 0.5f;
    quad.height = 0.5f;
    quad.texture = texture.value();
    REQUIRE(renderer.value()->set_overlay_quads({quad}).ok());

    // The renderer was created for sRGB (Linear). The UNORM target selects the
    // pipelines built for its counterpart; the setting is the only difference.
    const VkFormat gamma_format = splat::blend_alt_format(options.target_format);
    REQUIRE(gamma_format != options.target_format);
    auto gamma_image = splat::create_vulkan_color_image(
        device, gamma_format, renderer.value()->target_extent(),
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    REQUIRE_MESSAGE(gamma_image.ok(), gamma_image.message());

    splat::RenderTarget target;
    target.image = gamma_image.value().image;
    target.view = gamma_image.value().view;
    target.extent = renderer.value()->target_extent();
    target.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    target.format = gamma_format;
    REQUIRE_MESSAGE(renderer.value()->render(splat::FrameView{}, target, true).ok(),
                    "gamma render");
    auto pixels = renderer.value()->read_image();
    REQUIRE_MESSAGE(pixels.ok(), pixels.message());
    const uint8_t* centre = pixels.value().pixel(32, 32);
    MESSAGE("gamma target centre " << static_cast<int>(centre[0]) << ","
                                   << static_cast<int>(centre[1]) << ","
                                   << static_cast<int>(centre[2]));
    CHECK(centre[0] >= 200);
    CHECK(centre[1] >= 200);
    CHECK(centre[2] >= 200);
    splat::destroy_vulkan_image(device, gamma_image.value());
}

#else

TEST_CASE("scene label overlay is skipped without Vulkan") { CHECK(true); }

#endif
