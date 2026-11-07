// The TV's view of a VR session (0.6.0, "one menu, two views"): the scene as
// the left eye sees it, cropped to the TV's 16:9, with no VR window, label or
// HUD in it, plus the same menu canvas drawn flat and fixed on the TV screen
// while the menu is open.
//
// The Hmd2 mirror can only show the eye buffers (hmd2.cpp mirror_start: one
// option word and two work areas), so the TV gets its own picture: the renderer
// copies this crop out of the frame between the comfort pass and the overlay
// pass (SplatRenderer::request_tv_frame), draws the TV quads onto the copy and
// reads it back for the title's VideoOut port. This file is the geometry only,
// no Vulkan objects, so the desktop test checks the maths.
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

// The TV image is a 1920x1080 VideoOut frame (videoout.h takes 1920x1080,
// 2560x1440 and 3840x2160 and scales to the screen).
inline constexpr uint32_t kTvWidth = 1920;
inline constexpr uint32_t kTvHeight = 1080;

// A pixel rectangle of the left eye image, [x0, x1) x [y0, y1).
struct TvCrop {
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t width() const { return x1 - x0; }
    int32_t height() const { return y1 - y0; }
    bool valid() const { return x1 > x0 && y1 > y0; }
};

// The largest `out_w : out_h` region of one eye image centred on that eye's
// principal point (cx, cy), in eye pixels. Centred on the optical axis, not on
// the image: the PSVR2 eye frustum is asymmetric (1.842 out / 0.947 in), so the
// left eye's axis sits at x = 1309 of 1984 and an image-centred crop would look
// off to one side. An axis near the border makes the crop smaller, never
// off-axis. An empty eye or output gives an invalid crop.
TvCrop tv_crop(float cx, float cy, uint32_t eye_w, uint32_t eye_h, uint32_t out_w = kTvWidth,
               uint32_t out_h = kTvHeight);

#if defined(SPLAT_HAVE_VULKAN)
// The camera the TV quads are projected with: at the origin looking down +z,
// a 60 degree horizontal pinhole over the TV image. It belongs to the TV pass
// only; nothing in the scene shares it.
OverlayCamera tv_overlay_camera(uint32_t width = kTvWidth, uint32_t height = kTvHeight);

// A quad covering the pixel rectangle (x, y, w, h) of the TV image (x right, y
// down, 0,0 the top-left corner) as seen by `camera`. For a banner or plate;
// the menu itself is tv_panel_quad (ui_panel.h), which fills the whole image.
OverlayQuad tv_screen_quad(const OverlayCamera& camera, float x, float y, float w, float h);
#endif

}  // namespace splat
