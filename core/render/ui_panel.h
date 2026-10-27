// Panel placement for the in-app UI (docs/ui-v1-plan.md section 2): the quad
// the CPU-drawn UiCanvas is uploaded to.
//
// Geometry only, no Vulkan objects: the maths is host-checked without a device.
// OverlayCamera lives in render/overlay.h, which is compiled only when the
// project builds its Vulkan renderer, so the TV helper follows the same guard
// as the renderer (the VR helper needs camera.h only).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "render/camera.h"
#include "render/overlay_quad.h"

#if defined(SPLAT_HAVE_VULKAN)
#include "render/overlay.h"
#endif

namespace splat {

#if defined(SPLAT_HAVE_VULKAN)
// A world space quad that fills one mono TV view exactly: the canvas texture
// spans the viewport corner to corner. `camera` is the same OverlayCamera the
// renderer hands its overlay pass for that eye, so the panel and the splats
// share one projection. A camera without valid intrinsics falls back to the
// renderer's 60 degree horizontal field of view. With no viewport size at all
// (camera and canvas both zero) the quad comes back empty (width and height 0)
// and the caller skips it.
OverlayQuad tv_panel_quad(const OverlayCamera& camera, uint32_t canvas_w,
                          uint32_t canvas_h);
#endif

// The VR panel: a world-locked, yaw-only quad `distance` ahead of the
// reference pose, centred at the reference's eye height. Pitch and roll are
// ignored so the panel stays upright when the head (or the reference it was
// placed from) leans; its front faces the reference position. `width` and
// `height` are metres (1.4 x 0.7875 is 16:9 at reading size).
//
// This is the pre-VR8 placement, kept as the reference the new lazy follower
// (PanelFollower, hmd_view.h) replaced: a fixed yaw-only plane seen off-axis is
// exactly the keystoned panel of the 2026-10-27 recording. UiHost uses the
// follower; this function remains for the geometry test and the TV-style quad.
OverlayQuad vr_panel_quad(const CameraPose& reference, float distance = 1.6f,
                          float width = 1.4f, float height = 0.7875f);

}  // namespace splat
