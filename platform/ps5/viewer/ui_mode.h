// The VR session's panel state machine (W16): who owns a frame's input, when
// the panel opens and closes, and what the scene may still do while it is up.
//
// The panel itself is UiApp over UiHost, exactly as on the TV (ui_setup.h,
// core/ui). This file is only the routing: Options toggles the panel, an open
// panel takes every button and the left stick through pad::ui_actions_for (the
// one input map, pad_input.h), and the scene's own controls - Cross hold,
// sticks, L2/R2, L1/R1 - are suspended while it is up. The scene keeps being
// rendered behind the panel; only its input stops, so a stick that is held when
// the panel opens cannot keep walking the viewer behind it.
//
// Pure and header-only, no Vulkan and no pads: vr_mode.cpp feeds it one frame's
// button word and left stick, tests/vr_ui_state_test.cpp drives the same code
// with synthetic words (the console's own input path is what pad_input_test and
// the headset run prove).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "pad_input.h"

namespace viewer {

// One frame's routing decision.
struct UiRoute {
    // What the host's visibility should be this frame (UiHost::set_visible).
    bool panel_visible = false;
    // The panel takes this frame: ui_pressed/ui_held go to its UiInput.
    bool panel_input = false;
    // The scene's controls run this frame (recentre hold, sticks, L2/R2, L1/R1).
    // False whenever the panel is up, including the frame it opens on.
    bool scene_controls = false;
    // The panel was opened by this frame's Options edge (for the log line).
    bool opened = false;
    // splat::UiAction bits for UiInput, built from the same button word and left
    // stick the scene uses.
    uint32_t ui_pressed = 0;
    uint32_t ui_held = 0;
};

// The panel's open/close state and its input routing.
//
// `available` is viewer.env's UI: with UI=0 the panel does not exist in VR and
// every frame goes to the scene, exactly as before W16. `open` starts the
// session with the panel up (UI=1); the first frame then seeds the previous
// word instead of reporting presses, so a button held from the launch screen is
// not a press on the panel.
//
// The open edge is Options. Inside the panel Options is the panel's own (it
// opens Settings). The close edge is not an input edge: UiApp emits
// ResumeScene when Circle is pressed at the panel's root (core/ui/app.h), or
// ViewScene for a scene picked there, and the host calls close() with the
// change of scene it was asked for, so no press both closes and reopens it.
class PanelMode {
public:
    PanelMode() = default;
    explicit PanelMode(bool available, bool open = false) : available_(available), open_(open) {}

    bool available() const { return available_; }
    bool open() const { return open_; }

    // The panel is gone (ResumeScene, a scene loaded from it, or a failure):
    // the next route() gives the frame back to the scene.
    void close() { open_ = false; }

    // One frame. `buttons` is the frame's raw PS5 button word (the OR of every
    // pad, pad_input.h's layout), `left_x`/`left_y` the combined left stick,
    // right- and up-positive.
    UiRoute route(uint32_t buttons, float left_x = 0.0f, float left_y = 0.0f) {
        UiRoute out;
        const uint32_t actions = pad::ui_actions_for(buttons, left_x, left_y);
        const bool menu = (actions & splat::Menu) != 0;
        const bool menu_edge = menu && (previous_ & splat::Menu) == 0;

        if (!available_) {
            previous_ = actions;
            seen_ = true;
            out.scene_controls = true;
            return out;
        }
        if (!open_) {
            if (seen_ && menu_edge) {
                open_ = true;
                out.opened = true;
                out.panel_visible = true;
                // The opening press belongs to neither side: the panel must not
                // read it as its own Menu (which would open Settings at once).
                previous_ = actions;
                return out;
            }
            previous_ = actions;
            seen_ = true;
            out.scene_controls = true;
            return out;
        }
        out.panel_visible = true;
        out.panel_input = true;
        out.ui_held = actions;
        out.ui_pressed = seen_ ? (actions & ~previous_) : 0;
        previous_ = actions;
        seen_ = true;
        return out;
    }

private:
    bool available_ = true;
    bool open_ = false;
    uint32_t previous_ = 0;  // last frame's ui_actions_for word
    bool seen_ = false;      // a previous word exists (no press on the first frame)
};

}  // namespace viewer
