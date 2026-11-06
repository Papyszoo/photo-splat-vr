// When the VR frame has to rewrite the overlay's quad list.
//
// The overlay is one vertex buffer (SplatRenderer::set_overlay_quads): the scene
// label, the head-locked HUD, the scale pivot marker and the panel. Most of it
// only changes on an edge - the label when the scene moves, a HUD element when
// it appears or disappears - so the frame loop skips the rebuild while nothing
// changes. The panel is the exception: UiHost rebuilds its quads from its world
// anchor every frame, and a recentre (the Cross hold with the panel open)
// changes that anchor with no other edge on that frame, so the list has to be
// pushed every frame the panel is visible. Missing that term leaves the drawn
// panel at the old anchor while the pointer ray tests the new panel_quad() (the
// VR8 review's defect 1, still true for the recentre frame).
//
// Pure and header-only, like ui_mode.h: vr_mode.cpp feeds it one frame's flags
// and tests/vr8_test.cpp drives the same header on the desktop.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace viewer {

struct OverlayRefresh {
    // The VR panel is visible this frame: its quads are rebuilt from the world
    // anchor every frame, so a recentre reaches the drawn quads.
    bool panel_visible = false;
    // A HUD element (hold bar, scale readout) is visible this frame.
    bool hud_visible = false;
    // The previous frame had a HUD element: the frame it goes away still has to
    // rebuild the list without it.
    bool hud_shown = false;
    // The scene's scale or offset differs from the last uploaded list.
    bool moved = false;
    // A texture or label changed since the last upload.
    bool dirty = false;
    // The panel opened or closed on this frame.
    bool panel_changed = false;
    // The scale pivot marker appeared or went away on this frame.
    bool marker_changed = false;
};

inline bool overlay_needs_update(const OverlayRefresh& frame) {
    return frame.panel_visible || frame.hud_visible || frame.hud_shown || frame.moved ||
           frame.dirty || frame.panel_changed || frame.marker_changed;
}

}  // namespace viewer
