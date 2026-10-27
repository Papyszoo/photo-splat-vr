// The viewer's head-locked HUD: the recentre-hold progress bar with its
// "Hold to recentre" line, and the "Scale 1.6x" readout.
//
// Geometry only (no Vulkan): the caller owns the R8 text masks and the 1x1
// solid texture, and passes their indices in. Both elements sit on a plane
// `distance` metres in front of the eye and follow the head in all six degrees
// of freedom, so they stay just below (hold) and above (scale) the view centre
// however the user looks around. The quads are in the placement frame, which is
// the frame the overlay pass projects with - the same camera as the splats.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/camera.h"
#include "render/overlay_quad.h"

namespace splat {

// Both strings are a fixed number of characters, so their masks keep one size
// while the scale text changes and the texture update is a same-size upload.
inline constexpr const char* kHoldHudText = "Hold to recentre";
inline constexpr size_t kHoldHudChars = 16;
// "Scale 1.0x - front" / "Scale 0.3x - centre": 18-19 characters at every
// scale and mode; the mask is sized for the longer one (the centre wording).
inline constexpr size_t kScaleHudChars = 19;

struct HudLayout {
    // The plane's distance from the eye, metres.
    float distance = 1.5f;
    // Laid-out height of one 8x8 glyph cell, metres. The text mask is rasterised
    // at `pixel_scale` texels per font pixel; hud_text_width()/height() and
    // rasterise_text() must be given the same scale.
    float glyph_height = 0.022f;
    uint32_t pixel_scale = 4;
    float padding = 0.012f;
    // Gap between the text line and the bar, and the bar's size, metres.
    float bar_gap = 0.010f;
    float bar_width = 0.30f;
    float bar_height = 0.010f;
    // How far below / above the view centre each element sits, metres.
    float hold_offset = 0.075f;
    float scale_offset = 0.075f;

    // Straight-alpha tints (linear RGB). The plate is the dark backing both
    // elements share; the bar's track is dimmer than its filled part.
    float plate_tint[4] = {0.03f, 0.03f, 0.035f, 0.62f};
    float text_tint[4] = {0.94f, 0.95f, 0.97f, 0.98f};
    float bar_track_tint[4] = {0.62f, 0.64f, 0.68f, 0.35f};
    float bar_fill_tint[4] = {0.36f, 0.80f, 0.47f, 0.95f};
};

// What to draw this frame. `hold_progress` below zero hides the hold element;
// `show_scale` hides the readout when false.
struct HudState {
    float hold_progress = -1.0f;
    bool show_scale = false;
};

// Texture indices from SplatRenderer::add_overlay_texture. UINT32_MAX hides an
// element whose texture is missing.
struct HudTextures {
    uint32_t hold_text = UINT32_MAX;
    uint32_t scale_text = UINT32_MAX;
    uint32_t solid = UINT32_MAX;   // 1x1 white, for the plate and the bar
};

// One HUD text line's mask size in texels, for rasterise_text().
uint32_t hud_text_pixels(size_t characters, uint32_t pixel_scale);
uint32_t hud_text_line_pixels(uint32_t pixel_scale);
// The laid-out size of a line, metres.
float hud_text_width(size_t characters, const HudLayout& layout);
float hud_text_height(const HudLayout& layout);

// The quads for one frame, in front of `camera`: plate first, then the text /
// bar, so the caller can append them to the scene label's quads in one list.
// Empty when nothing is visible or the solid texture is missing.
std::vector<OverlayQuad> hud_quads(const CameraPose& camera, const HudState& state,
                                   const HudLayout& layout, const HudTextures& textures);

}  // namespace splat
