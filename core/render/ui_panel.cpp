// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ui_panel.h"

#include <cmath>

namespace splat {

#if defined(SPLAT_HAVE_VULKAN)
namespace {

// The panel's distance from the TV eye. Any positive distance projects to the
// same rectangle: the width grows with the distance, the pixel span does not.
constexpr float kTvPanelDistance = 1.0f;
constexpr float kDefaultHorizontalFovDegrees = 60.0f;

// The camera axes and eye position from the world -> camera matrix the overlay
// pass uses (column-major, OpenCV axes: +x right, +y down, +z forward). The
// rows of the rotation are the camera axes in world space and the camera
// position is -R^T t.
void camera_from_view(const float view[16], float right[3], float down[3],
                      float forward[3], float position[3]) {
    // view is column-major: view[column * 4 + row]. Row 0 is the camera's right
    // axis in world space, row 1 down, row 2 forward.
    for (int axis = 0; axis < 3; ++axis) {
        right[axis] = view[axis * 4 + 0];
        down[axis] = view[axis * 4 + 1];
        forward[axis] = view[axis * 4 + 2];
    }
    for (int axis = 0; axis < 3; ++axis) {
        position[axis] = -(right[axis] * view[12] + down[axis] * view[13] +
                           forward[axis] * view[14]);
    }
}

}  // namespace

OverlayQuad tv_panel_quad(const OverlayCamera& camera, uint32_t canvas_w,
                          uint32_t canvas_h) {
    // The overlay shader maps a camera-space point (x, y, z) to pixels
    //   px = fx * (x - eye_offset) / z + cx + cx_shift,  py = fy * y / z + cy.
    // A quad at z = d fills the viewport (0, 0)..(W, H) when its half extents
    // are d * W / (2 fx) and d * H / (2 fy), and its centre is at camera-space
    //   X = eye_offset + d * (W / 2 - cx - cx_shift) / fx,  Y = d * (H / 2 - cy) / fy.
    // The principal point can be off centre (a TV's scaled capture intrinsics),
    // so the centre is shifted to keep all four corners on the viewport edges.
    // Aspect follows the viewport, not the canvas; a 16:9 canvas on a 16:9 TV
    // therefore lands one canvas pixel per output pixel times the TV scale.
    const uint32_t width = camera.width != 0 ? camera.width : canvas_w;
    const uint32_t height = camera.height != 0 ? camera.height : canvas_h;
    // No viewport to fill (a default-constructed camera and canvas): return an
    // empty quad and let the caller skip it. The fallback below would divide by
    // a zero fx and produce NaN.
    if (width == 0 || height == 0) {
        OverlayQuad empty;
        empty.width = 0.0f;
        empty.height = 0.0f;
        return empty;
    }
    // The same guard for degenerate intrinsics (a negative or zero focal
    // length is not "invalid" only by accident: valid() already requires > 0).
    CameraIntrinsics intrinsics = camera.intrinsics;
    if (!intrinsics.valid()) {
        const float fx = 0.5f * static_cast<float>(width) /
                         std::tan(0.5f * kDefaultHorizontalFovDegrees *
                                  3.14159265358979323846f / 180.0f);
        intrinsics = {fx, fx, 0.5f * static_cast<float>(width),
                      0.5f * static_cast<float>(height)};
    }

    float right[3], down[3], forward[3], position[3];
    camera_from_view(camera.view, right, down, forward, position);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float z = kTvPanelDistance;
    const float half_width = z * w / (2.0f * intrinsics.fx);
    const float half_height = z * h / (2.0f * intrinsics.fy);
    const float centre_x =
        camera.eye_offset + z * (0.5f * w - intrinsics.cx - camera.cx_shift) / intrinsics.fx;
    const float centre_y = z * (0.5f * h - intrinsics.cy) / intrinsics.fy;

    OverlayQuad quad;
    for (int axis = 0; axis < 3; ++axis) {
        quad.center[axis] = position[axis] + right[axis] * centre_x + down[axis] * centre_y +
                            forward[axis] * z;
        quad.right[axis] = right[axis];
        quad.up[axis] = -down[axis];
    }
    quad.width = 2.0f * half_width;
    quad.height = 2.0f * half_height;
    return quad;
}
#endif  // SPLAT_HAVE_VULKAN

OverlayQuad vr_panel_quad(const CameraPose& reference, float distance, float width,
                          float height) {
    // Yaw only: the panel's right axis is the level right of the reference's
    // yaw and its up axis is world up (-y, the scene's y is down), so a rolled
    // or pitched reference still faces an upright panel. It looks back at the
    // reference: right x up = -forward.
    const float cy = std::cos(reference.yaw);
    const float sy = std::sin(reference.yaw);
    const float forward[3] = {sy, 0.0f, cy};
    const float right[3] = {cy, 0.0f, -sy};

    OverlayQuad quad;
    for (int axis = 0; axis < 3; ++axis) {
        quad.center[axis] = reference.position[axis] + forward[axis] * distance;
        quad.right[axis] = right[axis];
        quad.up[axis] = axis == 1 ? -1.0f : 0.0f;
    }
    quad.width = width;
    quad.height = height;
    return quad;
}

}  // namespace splat
