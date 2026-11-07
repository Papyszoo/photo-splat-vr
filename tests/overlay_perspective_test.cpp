// The world-fixed VR window is a rigid object: its corners and every content
// texel have fixed world positions for any head pose, and each eye sees them
// through ONE plane-to-image homography derived from the world quad alone
// (docs/ui-v1-plan.md "Panel placement in VR"). Head movement may change where
// that quad sits in the view; it must not warp how the content maps onto it.
//
// Two parts:
//
//   - CPU only (every desktop build): the panel anchor's world corners are
//     bit-identical across head poses, and for each pose and each eye the
//     homography fitted to the four projected corners predicts the projection of
//     a grid of content points (the pinhole model overlay.vert implements, with
//     the per-eye shift and principal-point offset).
//   - GPU (needs a Vulkan device; skips with a message without one): the same
//     quad is drawn by the real overlay pass in stereo, with a texture whose red
//     and green channels are the texel's x and y, and every pixel inside the
//     quad has to carry the texel that homography says lands there. Corners
//     alone cannot tell: the old overlay.vert wrote w = 1, so the rasteriser
//     interpolated the texture coordinates linearly in screen space across the
//     quad's two triangles, and an obliquely seen window (which a world-fixed
//     one is whenever the head turns) showed its content skewed along the
//     diagonal while all four corners were still exactly right.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/camera.h"
#include "render/hmd_view.h"
#include "render/overlay_quad.h"

#if defined(SPLAT_HAVE_VULKAN)

#include "common/image.h"
#include "render/splat_renderer.h"
#include "render/vulkan_context.h"
#include "scene/splat_scene.h"

#ifndef SPLAT_SHADER_DIR
#error "SPLAT_SHADER_DIR must point at the compiled SPIR-V directory"
#endif

#endif  // SPLAT_HAVE_VULKAN

namespace {

constexpr double kPi = 3.14159265358979323846;

// One stereo frame: eye size, lens, per-eye principal-point offsets (the PSVR2
// frustum is not centred on each panel) and the eye separation.
constexpr int kEyeWidth = 512;
constexpr int kEyeHeight = 384;
constexpr double kFocal = 256.0;  // 90 degrees across the eye width
constexpr double kEyeShift[2] = {40.0, -40.0};
constexpr double kHalfIpd = 0.032;

struct Mat3 {
    double m[9] = {};
};

// The renderer's world -> camera matrix for a CameraPose (splat_renderer.cpp;
// the same convention ui_panel_test.cpp and vr8_test.cpp use), in double.
std::array<double, 16> view_from_pose(const splat::CameraPose& pose) {
    const double cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const double cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const double f[3] = {sy * cp, sp, cy * cp};
    double s[3] = {cy, 0.0, -sy};
    double d[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
                   f[0] * s[1] - f[1] * s[0]};
    const double cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    const double s0[3] = {s[0], s[1], s[2]};
    const double d0[3] = {d[0], d[1], d[2]};
    for (int axis = 0; axis < 3; ++axis) {
        s[axis] = cr * s0[axis] + sr * d0[axis];
        d[axis] = cr * d0[axis] - sr * s0[axis];
    }
    const float* p = pose.position;
    std::array<double, 16> view{};
    for (int axis = 0; axis < 3; ++axis) {
        view[static_cast<size_t>(axis * 4 + 0)] = s[axis];
        view[static_cast<size_t>(axis * 4 + 1)] = d[axis];
        view[static_cast<size_t>(axis * 4 + 2)] = f[axis];
    }
    view[12] = -(s[0] * p[0] + s[1] * p[1] + s[2] * p[2]);
    view[13] = -(d[0] * p[0] + d[1] * p[1] + d[2] * p[2]);
    view[14] = -(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
    view[15] = 1.0;
    return view;
}

struct Pixel {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;  // camera-space z of the point, metres
};

// World point -> one eye's pixel: overlay.vert's pinhole model. Eye 0 is the
// left eye (shifted -ipd/2 along camera x), eye 1 the right one.
Pixel project(const std::array<double, 16>& view, int eye, const double world[3]) {
    const double x = view[0] * world[0] + view[4] * world[1] + view[8] * world[2] + view[12];
    const double y = view[1] * world[0] + view[5] * world[1] + view[9] * world[2] + view[13];
    const double z = view[2] * world[0] + view[6] * world[1] + view[10] * world[2] + view[14];
    const double eye_x = x - (eye == 0 ? -kHalfIpd : kHalfIpd);
    return {kFocal * eye_x / z + 0.5 * kEyeWidth + kEyeShift[eye],
            kFocal * y / z + 0.5 * kEyeHeight, z};
}

// The world position of the panel texel at (u, v), v = 0 the top edge.
void content_point(const splat::OverlayQuad& quad, double u, double v, double out[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = quad.center[axis] + (u - 0.5) * quad.width * quad.right[axis] +
                    (0.5 - v) * quad.height * quad.up[axis];
    }
}

// The homography (content (u, v) -> pixel) fitted to four corner
// correspondences alone: the direct linear transform with h33 = 1, solved by
// Gaussian elimination. This is the only thing the test knows about the screen
// besides the pinhole model; interior content points must agree with it.
Mat3 fit_homography(const double src[4][2], const double dst[4][2]) {
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double u = src[i][0], v = src[i][1], x = dst[i][0], y = dst[i][1];
        const double r0[9] = {u, v, 1, 0, 0, 0, -u * x, -v * x, x};
        const double r1[9] = {0, 0, 0, u, v, 1, -u * y, -v * y, y};
        for (int k = 0; k < 9; ++k) {
            a[2 * i][k] = r0[k];
            a[2 * i + 1][k] = r1[k];
        }
    }
    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        }
        for (int k = 0; k < 9; ++k) std::swap(a[col][k], a[pivot][k]);
        for (int row = 0; row < 8; ++row) {
            if (row == col) continue;
            const double factor = a[row][col] / a[col][col];
            for (int k = col; k < 9; ++k) a[row][k] -= factor * a[col][k];
        }
    }
    Mat3 h;
    for (int i = 0; i < 8; ++i) h.m[i] = a[i][8] / a[i][i];
    h.m[8] = 1.0;
    return h;
}

Pixel apply(const Mat3& h, double u, double v) {
    const double w = h.m[6] * u + h.m[7] * v + h.m[8];
    return {(h.m[0] * u + h.m[1] * v + h.m[2]) / w, (h.m[3] * u + h.m[4] * v + h.m[5]) / w, w};
}

Mat3 inverse(const Mat3& h) {
    const double* m = h.m;
    const double c0 = m[4] * m[8] - m[5] * m[7];
    const double c1 = m[5] * m[6] - m[3] * m[8];
    const double c2 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    Mat3 r;
    r.m[0] = c0 / det;
    r.m[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    r.m[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    r.m[3] = c1 / det;
    r.m[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    r.m[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    r.m[6] = c2 / det;
    r.m[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    r.m[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return r;
}

// The homography of the fixed quad, for one head pose and eye, from its four
// projected corners (u, v) = (0,0) (1,0) (1,1) (0,1).
Mat3 panel_homography(const splat::OverlayQuad& quad, const std::array<double, 16>& view,
                      int eye) {
    const double src[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    double dst[4][2];
    for (int i = 0; i < 4; ++i) {
        double world[3];
        content_point(quad, src[i][0], src[i][1], world);
        const Pixel pixel = project(view, eye, world);
        REQUIRE(pixel.depth > 0.0);
        dst[i][0] = pixel.x;
        dst[i][1] = pixel.y;
    }
    return fit_homography(src, dst);
}

splat::CameraPose pose_of(float x, float y, float z, double yaw_deg, double pitch_deg,
                          double roll_deg) {
    splat::CameraPose pose;
    pose.position[0] = x;
    pose.position[1] = y;
    pose.position[2] = z;
    pose.yaw = static_cast<float>(yaw_deg * kPi / 180.0);
    pose.pitch = static_cast<float>(pitch_deg * kPi / 180.0);
    pose.roll = static_cast<float>(roll_deg * kPi / 180.0);
    return pose;
}

// Head poses the window is looked at from after it opened at the origin: the
// reference, pure turns (yaw, pitch, roll), small translations, and a turn plus
// a step sideways that views the panel 37 degrees off its normal.
std::vector<splat::CameraPose> head_poses() {
    return {
        pose_of(0.0f, 0.0f, 0.0f, 0.0, 0.0, 0.0),
        pose_of(0.0f, 0.0f, 0.0f, 22.0, 0.0, 0.0),
        pose_of(0.0f, 0.0f, 0.0f, -25.0, 12.0, 0.0),
        pose_of(0.0f, 0.0f, 0.0f, 15.0, -20.0, 8.0),
        pose_of(0.12f, -0.08f, 0.15f, 0.0, 0.0, 0.0),
        pose_of(-0.20f, 0.05f, -0.10f, 10.0, 5.0, -6.0),
        pose_of(-0.90f, -0.05f, 0.40f, 37.0, -3.0, 0.0),
        pose_of(1.10f, 0.10f, 0.30f, -40.0, 4.0, 5.0),
    };
}

}  // namespace

TEST_CASE("the world-fixed window keeps its world corners for every head pose") {
    splat::PanelAnchor panel;
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    panel.latch(eye, 0.0f, 0.0f);
    const splat::OverlayQuad opened = panel.quad();

    double opened_corners[4][3];
    const double src[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) content_point(opened, src[i][0], src[i][1], opened_corners[i]);

    for (const splat::CameraPose& head : head_poses()) {
        // UiHost::panel_quad's rule: latch only while unlatched. Whatever the
        // head does, the quad the frame draws is the stored one.
        if (!panel.latched()) panel.latch(head.position, head.yaw, head.pitch);
        const splat::OverlayQuad drawn = panel.quad();
        for (int i = 0; i < 4; ++i) {
            double corner[3];
            content_point(drawn, src[i][0], src[i][1], corner);
            for (int axis = 0; axis < 3; ++axis) CHECK(corner[axis] == opened_corners[i][axis]);
        }
        CHECK(drawn.width == opened.width);
        CHECK(drawn.height == opened.height);
    }
}

TEST_CASE("one homography per eye maps every content point of the fixed window") {
    splat::PanelAnchor panel;
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    panel.latch(origin, 0.0f, 0.0f);
    const splat::OverlayQuad quad = panel.quad();

    double worst = 0.0;
    int checked = 0;
    for (const splat::CameraPose& head : head_poses()) {
        const std::array<double, 16> view = view_from_pose(head);
        for (int eye = 0; eye < 2; ++eye) {
            const Mat3 h = panel_homography(quad, view, eye);
            // The homography fitted to the four corners predicts the pinhole
            // projection of the panel's interior: the centre, the edge midpoints
            // and a 5 x 5 grid. A head-dependent warp of the content would show
            // up here as a residual.
            for (int iu = 0; iu <= 4; ++iu) {
                for (int iv = 0; iv <= 4; ++iv) {
                    const double u = iu / 4.0, v = iv / 4.0;
                    double world[3];
                    content_point(quad, u, v, world);
                    const Pixel direct = project(view, eye, world);
                    const Pixel predicted = apply(h, u, v);
                    worst = std::max({worst, std::abs(direct.x - predicted.x),
                                      std::abs(direct.y - predicted.y)});
                    ++checked;
                }
            }
            // The panel's own mapping is invertible for every pose.
            const Pixel centre = apply(h, 0.5, 0.5);
            const Mat3 back = inverse(h);
            const Pixel round_trip = apply(back, centre.x, centre.y);
            CHECK(round_trip.x == doctest::Approx(0.5).epsilon(1e-6));
            CHECK(round_trip.y == doctest::Approx(0.5).epsilon(1e-6));
        }
    }
    MESSAGE(checked << " content points, worst residual " << worst << " px");
    CHECK(worst < 1e-6);
}

#if defined(SPLAT_HAVE_VULKAN)

namespace {

constexpr int kTexture = 128;

// Texel (x, y) -> red = 2x, green = 2y: the frame's colours read back as the
// content coordinate the pixel shows.
std::vector<uint8_t> coordinate_texture() {
    std::vector<uint8_t> rgba(static_cast<size_t>(kTexture) * kTexture * 4);
    for (int y = 0; y < kTexture; ++y) {
        for (int x = 0; x < kTexture; ++x) {
            uint8_t* p = rgba.data() + (static_cast<size_t>(y) * kTexture + x) * 4;
            p[0] = static_cast<uint8_t>(2 * x);
            p[1] = static_cast<uint8_t>(2 * y);
            p[2] = 0;
            p[3] = 255;
        }
    }
    return rgba;
}

}  // namespace

TEST_CASE("the drawn window shows every content point where the fixed-quad homography says") {
    auto context = splat::VulkanContext::create(splat::VulkanContextOptions{});
    if (!context.ok()) {
        MESSAGE("skipping: " << context.message());
        return;
    }
    splat::VulkanContext& device = *context.value();

    splat::RendererOptions options;
    options.shader_dir = SPLAT_SHADER_DIR;
    options.eye_width = kEyeWidth;
    options.eye_height = kEyeHeight;
    options.stereo = true;
    options.target_format = VK_FORMAT_R8G8B8A8_SRGB;
    options.intrinsics = {static_cast<float>(kFocal), static_cast<float>(kFocal),
                          0.5f * kEyeWidth, 0.5f * kEyeHeight};
    options.eye_center_shift[0] = static_cast<float>(kEyeShift[0]);
    options.eye_center_shift[1] = static_cast<float>(kEyeShift[1]);
    options.backdrop[0] = options.backdrop[1] = options.backdrop[2] = 0.0f;

    // One splat with no opacity far behind the origin: the frame is the backdrop
    // (black) plus the overlay, so every lit pixel is the window.
    splat::SplatScene scene;
    scene.resize(1);
    scene.position = {0.0f, 0.0f, -50.0f};
    scene.log_scale = {-4.0f, -4.0f, -4.0f};
    scene.rotation = {1.0f, 0.0f, 0.0f, 0.0f};
    scene.sh_dc = {0.5f, 0.5f, 0.5f};
    scene.opacity_logit = {-20.0f};
    REQUIRE(scene.consistent());

    auto renderer = splat::SplatRenderer::create(device, options, scene);
    REQUIRE_MESSAGE(renderer.ok(), renderer.message());
    const std::vector<uint8_t> texels = coordinate_texture();
    auto texture = renderer.value()->add_overlay_rgba_texture(kTexture, kTexture, texels.data());
    REQUIRE_MESSAGE(texture.ok(), texture.message());

    splat::PanelAnchor panel;
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    panel.latch(origin, 0.0f, 0.0f);
    splat::OverlayQuad quad = panel.quad();
    quad.texture = texture.value();
    REQUIRE_MESSAGE(renderer.value()->set_overlay_quads({quad}).ok(), "set_overlay_quads");

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

    int pose_index = 0;
    for (const splat::CameraPose& head : head_poses()) {
        splat::FrameView frame;
        frame.midpoint = head;
        frame.ipd = static_cast<float>(2.0 * kHalfIpd);
        const splat::Status status = renderer.value()->render(frame, target, true);
        REQUIRE_MESSAGE(status.ok(), status.message());
        target.initial_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        auto pixels = renderer.value()->read_image();
        REQUIRE_MESSAGE(pixels.ok(), pixels.message());
        const splat::Image& frame_image = pixels.value();

        const std::array<double, 16> view = view_from_pose(head);
        for (int eye = 0; eye < 2; ++eye) {
            const Mat3 back = inverse(panel_homography(quad, view, eye));
            double worst_texels = 0.0;
            int inside = 0;
            int stray = 0;
            for (int y = 0; y < kEyeHeight; ++y) {
                for (int x = 0; x < kEyeWidth; ++x) {
                    // The content coordinate the homography puts under this
                    // pixel's centre.
                    const Pixel uv = apply(back, x + 0.5, y + 0.5);
                    const uint8_t* got = frame_image.pixel(eye * kEyeWidth + x, y);
                    const bool in_panel = uv.x > 0.04 && uv.x < 0.96 && uv.y > 0.04 &&
                                          uv.y < 0.96;
                    const bool out_of_panel = uv.x < -0.04 || uv.x > 1.04 || uv.y < -0.04 ||
                                              uv.y > 1.04;
                    if (in_panel) {
                        // Red/green are 2 * the texel index: texel centre i + 0.5
                        // is at content u = (i + 0.5) / 128, so the read-back
                        // coordinate in texels is value / 2 + 0.5.
                        const double seen_u = (got[0] / 2.0 + 0.5);
                        const double seen_v = (got[1] / 2.0 + 0.5);
                        worst_texels = std::max({worst_texels,
                                                 std::abs(seen_u - uv.x * kTexture),
                                                 std::abs(seen_v - uv.y * kTexture)});
                        ++inside;
                    } else if (out_of_panel && (got[0] | got[1] | got[2]) != 0) {
                        ++stray;
                    }
                }
            }
            MESSAGE("pose " << pose_index << " eye " << eye << ": " << inside
                            << " window pixels, worst content error " << worst_texels
                            << " texels, " << stray << " pixels outside the window");
            // The window is on screen (the check is not vacuous) ...
            CHECK(inside > 3000);
            // ... every pixel carries the texel the fixed-quad homography puts
            // there (the gradient is 2 LSB per texel, so the 8-bit read-back alone
            // is worth up to 0.5 texel; the unfixed shader was 6 to 19 texels off
            // on every turned pose) ...
            CHECK(worst_texels < 1.0);
            // ... and nothing is drawn outside its frame.
            CHECK(stray == 0);
        }
        ++pose_index;
    }
    splat::destroy_vulkan_image(device, image.value());
}

#else

TEST_CASE("the window homography render test is skipped without Vulkan") { CHECK(true); }

#endif  // SPLAT_HAVE_VULKAN
