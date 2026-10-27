// TV and VR panel placement (core/render/ui_panel.h). The geometry is pure, so
// the desktop checks it without a headset:
//
//   - the VR quad is world locked ahead of the reference, upright when the
//     reference rolls, and its front faces the reference position (this runs in
//     every desktop build: vr_panel_quad needs camera.h only);
//   - the TV quad projects its four corners onto the viewport corners within
//     0.5 px, for centred and off-centre intrinsics, with a rotated camera and
//     the overlay pass's per-eye shift;
//   - the fallback (no intrinsics) is the renderer's 60 degree field of view;
//   - a zero camera/canvas size gives an empty quad, not NaN.
//
// The TV cases need splat::OverlayCamera, which render/overlay.h defines only
// for the Vulkan build (it is a Vulkan type); the case names say so and the
// no-Vulkan branch skips them with a message rather than hiding the whole file.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>

#include "render/camera.h"
#include "render/overlay_quad.h"
#include "render/ui_panel.h"

#if defined(SPLAT_HAVE_VULKAN)

#include "render/overlay.h"

namespace {

// The renderer's world -> camera matrix for a CameraPose (splat_renderer.cpp:
// yaw about world y, then pitch about the yawed right, then roll; column-major
// OpenCV axes). Kept here so the test projects with exactly the renderer's view.
void view_from_pose(const splat::CameraPose& pose, float view[16]) {
    const float cy = std::cos(pose.yaw);
    const float sy = std::sin(pose.yaw);
    const float cp = std::cos(pose.pitch);
    const float sp = std::sin(pose.pitch);
    const float f[3] = {sy * cp, sp, cy * cp};
    float s[3] = {cy, 0.0f, -sy};
    float d[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
                  f[0] * s[1] - f[1] * s[0]};
    const float cr = std::cos(pose.roll);
    const float sr = std::sin(pose.roll);
    const float level_s[3] = {s[0], s[1], s[2]};
    const float level_d[3] = {d[0], d[1], d[2]};
    for (int axis = 0; axis < 3; ++axis) {
        s[axis] = cr * level_s[axis] + sr * level_d[axis];
        d[axis] = cr * level_d[axis] - sr * level_s[axis];
    }
    const float* p = pose.position;
    view[0] = s[0];
    view[1] = d[0];
    view[2] = f[0];
    view[3] = 0.0f;
    view[4] = s[1];
    view[5] = d[1];
    view[6] = f[1];
    view[7] = 0.0f;
    view[8] = s[2];
    view[9] = d[2];
    view[10] = f[2];
    view[11] = 0.0f;
    view[12] = -(s[0] * p[0] + s[1] * p[1] + s[2] * p[2]);
    view[13] = -(d[0] * p[0] + d[1] * p[1] + d[2] * p[2]);
    view[14] = -(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
    view[15] = 1.0f;
}

splat::OverlayCamera make_camera(const splat::CameraPose& pose,
                                 const splat::CameraIntrinsics& intrinsics, uint32_t width,
                                 uint32_t height, float eye_offset = 0.0f,
                                 float cx_shift = 0.0f) {
    splat::OverlayCamera camera;
    view_from_pose(pose, camera.view);
    camera.intrinsics = intrinsics;
    camera.width = width;
    camera.height = height;
    camera.eye_offset = eye_offset;
    camera.cx_shift = cx_shift;
    return camera;
}

// overlay.vert's projection: camera-space point -> pixels.
void project(const splat::OverlayCamera& camera, const float point[3], double& px,
             double& py) {
    const float x = point[0] - camera.eye_offset;
    const float y = point[1];
    const float z = point[2];
    px = static_cast<double>(camera.intrinsics.fx) * x / z + camera.intrinsics.cx +
         camera.cx_shift;
    py = static_cast<double>(camera.intrinsics.fy) * y / z + camera.intrinsics.cy;
}

// Each TV quad corner, in canvas corner order: (+u,+v) top right, (-u,+v) top
// left, (-u,-v) bottom left, (+u,-v) bottom right.
void check_tv_corners(const splat::OverlayCamera& camera, const splat::OverlayQuad& quad) {
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float point[3];
            for (int axis = 0; axis < 3; ++axis) {
                point[axis] = quad.center[axis] +
                              0.5f * static_cast<float>(sx) * quad.width * quad.right[axis] +
                              0.5f * static_cast<float>(sy) * quad.height * quad.up[axis];
            }
            // World -> camera with the overlay camera's own view matrix.
            const float* v = camera.view;
            const float camera_point[3] = {
                v[0] * point[0] + v[4] * point[1] + v[8] * point[2] + v[12],
                v[1] * point[0] + v[5] * point[1] + v[9] * point[2] + v[13],
                v[2] * point[0] + v[6] * point[1] + v[10] * point[2] + v[14]};
            double px = 0.0;
            double py = 0.0;
            project(camera, camera_point, px, py);
            const double expected_x = sx > 0 ? camera.width : 0.0;
            // +v is the quad's top edge, which is pixel row 0.
            const double expected_y = sy > 0 ? 0.0 : camera.height;
            // Float round-off is ~1e-4 px here; the contract is 0.5 px.
            CHECK(std::abs(px - expected_x) <= 0.01);
            CHECK(std::abs(py - expected_y) <= 0.01);
            CHECK(std::abs(px - expected_x) <= 0.5);
            CHECK(std::abs(py - expected_y) <= 0.5);
        }
    }
}

}  // namespace

#endif  // SPLAT_HAVE_VULKAN

// The VR quad is pure geometry (camera.h + overlay_quad.h), so it is checked in
// a CPU-only desktop build too.
TEST_CASE("ui_panel VR quad is world locked and upright when the reference rolls") {
    splat::CameraPose reference;
    reference.position[0] = 0.4f;
    reference.position[1] = 1.6f;
    reference.position[2] = -0.8f;
    reference.yaw = 0.7f;
    reference.pitch = 0.45f;
    reference.roll = 0.9f;

    const float distance = 1.6f;
    const splat::OverlayQuad quad = splat::vr_panel_quad(reference, distance);

    CHECK(quad.width == doctest::Approx(1.4f));
    CHECK(quad.height == doctest::Approx(0.7875f));

    // Ahead of the reference along its level forward, at the same height.
    const float forward[3] = {std::sin(reference.yaw), 0.0f, std::cos(reference.yaw)};
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(quad.center[axis] ==
              doctest::Approx(reference.position[axis] + forward[axis] * distance));
    }
    CHECK(quad.center[1] == doctest::Approx(reference.position[1]));

    // Upright: world up (scene y is down), whatever the reference's pitch and
    // roll. Right is the level right of the yaw and square to up.
    CHECK(quad.up[0] == doctest::Approx(0.0f));
    CHECK(quad.up[1] == doctest::Approx(-1.0f));
    CHECK(quad.up[2] == doctest::Approx(0.0f));
    CHECK(quad.right[0] == doctest::Approx(std::cos(reference.yaw)));
    CHECK(quad.right[1] == doctest::Approx(0.0f));
    CHECK(quad.right[2] == doctest::Approx(-std::sin(reference.yaw)));
    CHECK(quad.right[0] * quad.up[0] + quad.right[1] * quad.up[1] +
              quad.right[2] * quad.up[2] ==
          doctest::Approx(0.0f));

    // Its front (right x up) faces the reference position, i.e. -forward.
    const float normal[3] = {
        quad.right[1] * quad.up[2] - quad.right[2] * quad.up[1],
        quad.right[2] * quad.up[0] - quad.right[0] * quad.up[2],
        quad.right[0] * quad.up[1] - quad.right[1] * quad.up[0]};
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(normal[axis] == doctest::Approx(-forward[axis]));
    }
    const double facing =
        normal[0] * forward[0] + normal[1] * forward[1] + normal[2] * forward[2];
    CHECK(facing == doctest::Approx(-1.0));

    // Pitch and roll do not move the panel: only the yaw and the position do.
    splat::CameraPose level = reference;
    level.pitch = 0.0f;
    level.roll = 0.0f;
    const splat::OverlayQuad level_quad = splat::vr_panel_quad(level, distance);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(quad.center[axis] == doctest::Approx(level_quad.center[axis]));
        CHECK(quad.right[axis] == doctest::Approx(level_quad.right[axis]));
        CHECK(quad.up[axis] == doctest::Approx(level_quad.up[axis]));
    }
}

#if defined(SPLAT_HAVE_VULKAN)

TEST_CASE("ui_panel TV quad fills a centred viewport corner to corner") {
    splat::CameraPose pose;
    const splat::CameraIntrinsics intrinsics{900.0f, 900.0f, 640.0f, 360.0f};
    const splat::OverlayCamera camera = make_camera(pose, intrinsics, 1280, 720);
    const splat::OverlayQuad quad = splat::tv_panel_quad(camera, 1280, 720);

    // Zero pose: the quad faces the camera from +z, up is world up.
    CHECK(quad.center[0] == doctest::Approx(0.0f));
    CHECK(quad.center[1] == doctest::Approx(0.0f));
    CHECK(quad.center[2] > 0.0f);
    CHECK(quad.right[0] == doctest::Approx(1.0f));
    CHECK(quad.right[2] == doctest::Approx(0.0f));
    CHECK(quad.up[0] == doctest::Approx(0.0f));
    CHECK(quad.up[1] == doctest::Approx(-1.0f));
    CHECK(quad.width == doctest::Approx(1280.0f / 900.0f));
    CHECK(quad.height == doctest::Approx(720.0f / 900.0f));
    check_tv_corners(camera, quad);
}

TEST_CASE("ui_panel TV quad fills the view through a rotated camera and shifted lens") {
    splat::CameraPose pose;
    pose.position[0] = 1.3f;
    pose.position[1] = 0.4f;
    pose.position[2] = -2.0f;
    pose.yaw = 0.6f;
    pose.pitch = -0.25f;
    pose.roll = 0.15f;
    // An off-centre principal point and the PSVR2-style per-eye shift: the
    // quad has to shift its centre, not just scale, or the corners miss.
    const splat::CameraIntrinsics intrinsics{1043.5f, 1041.25f, 601.5f, 372.25f};
    const splat::OverlayCamera camera =
        make_camera(pose, intrinsics, 1280, 720, 0.0315f, 157.0f);
    const splat::OverlayQuad quad = splat::tv_panel_quad(camera, 1280, 720);

    CHECK(quad.width == doctest::Approx(1280.0f / 1043.5f));
    CHECK(quad.height == doctest::Approx(720.0f / 1041.25f));
    check_tv_corners(camera, quad);
}

TEST_CASE("ui_panel TV quad without intrinsics is the 60 degree fallback") {
    splat::CameraPose pose;
    const splat::CameraIntrinsics no_intrinsics{};
    const splat::OverlayCamera camera = make_camera(pose, no_intrinsics, 1280, 720);
    const splat::OverlayQuad quad = splat::tv_panel_quad(camera, 1280, 720);

    // The renderer's resolve_intrinsics(): fx = (W / 2) / tan(30 deg), so the
    // full width at the panel distance is 2 * tan(30 deg) metres. Project with
    // those intrinsics, the ones the renderer would actually use.
    const double expected = 2.0 * std::tan(30.0 * 3.14159265358979323846 / 180.0);
    const float fx = 0.5f * 1280.0f / std::tan(30.0f * 3.14159265f / 180.0f);
    const splat::CameraIntrinsics fallback{fx, fx, 640.0f, 360.0f};
    CHECK(quad.width == doctest::Approx(static_cast<float>(expected)));
    CHECK(quad.center[0] == doctest::Approx(0.0f));
    check_tv_corners(make_camera(pose, fallback, 1280, 720), quad);
}

TEST_CASE("ui_panel TV quad with a zero camera and canvas size is empty, not NaN") {
    // A default OverlayCamera: no viewport, no intrinsics. The panel has
    // nothing to fill, so the quad is zero-sized (the host skips it) and every
    // value stays finite; the old code divided 0 by the fallback fx = 0.
    const splat::OverlayCamera camera;
    const splat::OverlayQuad quad = splat::tv_panel_quad(camera, 0, 0);
    CHECK(quad.width == 0.0f);
    CHECK(quad.height == 0.0f);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(std::isfinite(quad.center[axis]));
        CHECK(std::isfinite(quad.right[axis]));
        CHECK(std::isfinite(quad.up[axis]));
    }

    // One of the two sizes missing is the same case.
    const splat::OverlayQuad no_height = splat::tv_panel_quad(camera, 1280, 0);
    CHECK(no_height.width == 0.0f);
    CHECK(no_height.height == 0.0f);
    for (int axis = 0; axis < 3; ++axis) CHECK(std::isfinite(no_height.center[axis]));

    // The camera's own size is used when it has one, so the canvas size alone
    // being zero still fills a real viewport.
    const splat::CameraIntrinsics intrinsics{900.0f, 900.0f, 640.0f, 360.0f};
    const splat::OverlayQuad sized =
        splat::tv_panel_quad(make_camera(splat::CameraPose{}, intrinsics, 1280, 720), 0, 0);
    CHECK(sized.width == doctest::Approx(1280.0f / 900.0f));
    CHECK(sized.height == doctest::Approx(720.0f / 900.0f));
}

#else

// tv_panel_quad takes splat::OverlayCamera, which only the Vulkan build
// defines; there is no device involved, so nothing is compiled out silently.
TEST_CASE("ui_panel TV quad cases are skipped without the Vulkan overlay camera") {
    MESSAGE("skipping: built without Vulkan, so render/overlay.h has no OverlayCamera "
            "and tv_panel_quad does not exist; the VR cases above still ran");
    CHECK(true);
}

#endif  // SPLAT_HAVE_VULKAN
