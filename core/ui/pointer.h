// The Sense laser pointer and its ray test for the panel (docs/ui-v1-plan.md
// section 2: "right-controller laser ray hovers, R2 trigger = click").
//
// Everything here is pure maths on plain arrays, so the desktop test drives it
// with scripted poses and no headset: a ray against the panel's world-space
// OverlayQuad, the tracker-space controller pose turned into that same scene
// frame, and the deterministic smoothing the pointer position goes through
// before a screen sees it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

#include "render/camera.h"
#include "render/overlay_quad.h"

namespace splat {

// One hit point on the panel, in canvas pixels: (0, 0) is the canvas's top-left
// corner and (canvas_w, canvas_h) its bottom-right, the same coordinates
// UiInput::px/py carry.
struct PanelHit {
    float x = 0.0f;
    float y = 0.0f;
};

// Where a ray meets a panel quad. The quad's plane is its centre and normal
// (cross(right, up)); the hit must be ahead of the ray's origin (t > 0) and
// inside the quad's width and height. A parallel ray, a ray that points away,
// a miss outside the rectangle, or a zero-sized canvas gives no hit.
std::optional<PanelHit> ray_panel_hit(const float origin[3], const float dir[3],
                                      const OverlayQuad& panel, uint32_t canvas_w,
                                      uint32_t canvas_h);

// A controller's laser in the panel's scene frame.
//
// The tracker poses are Hmd2's (hmd2::Pose, the same block HLA-PSVR2 reads at
// psvr2.c:683-690, 827-870): metres, +y up, -z forward, quaternion (x, y, z, w)
// mapping the device's local axes into tracker space. The scene is OpenCV
// (render/camera.h): +x right, +y down, +z forward. The controller is carried
// through the head's own tracker -> scene transform, so the point it aims at is
// where the user actually points it, independent of the scene's placement,
// scale, offset and turn: the head's scene pose is the renderer's CameraPose
// for the same frame (FrameView::midpoint).
struct PointerRay {
    float origin[3] = {0.0f, 0.0f, 0.0f};   // scene metres
    float direction[3] = {0.0f, 0.0f, 1.0f};  // scene unit vector
};

PointerRay controller_ray(const float head_pos[3], const float head_quat[4],
                          const float controller_pos[3], const float controller_quat[4],
                          const CameraPose& head_scene);

// Exponential smoothing for the pointer position ("1-euro-style", made
// deterministic: a fixed time constant and the frame dt, no speed-dependent
// cutoff). The first sample after a reset is taken as-is, so a fresh ray does
// not glide in from the canvas corner; a frame without a hit clears the filter,
// which is what makes `pointer_valid` false when the ray leaves the panel.
class PointerFilter {
public:
    explicit PointerFilter(float time_constant_seconds = 0.08f);

    std::optional<PanelHit> update(const std::optional<PanelHit>& hit, double dt_seconds);
    void reset();

    bool valid() const { return valid_; }
    PanelHit value() const { return PanelHit{x_, y_}; }

private:
    float time_constant_ = 0.08f;
    bool valid_ = false;
    float x_ = 0.0f;
    float y_ = 0.0f;
};

}  // namespace splat
