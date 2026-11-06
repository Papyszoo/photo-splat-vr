// The panel's pointer mode (U3): the joystick drives the menu, R2 arms the
// Sense laser, the joystick turns it off again.
//
// The user, 2026-11-06: "When I'm using a joystick - I don't want sense
// controllers to fight with me. If I want to use sense controllers to point at
// something - it should be activated with R2 button, just like on meta OS.
// Currently it's really hard to scroll by moving sense controller and it's way
// easier and intuitive with joystick but because pointing fought with my
// selection I couldn't scroll with a joystick."
//
// So the panel starts on the stick every time it opens: no ray, and controller
// motion changes no focus. R2 arms the ring, and the press that arms it does
// not click and is not a drag level; the next R2 press clicks what the ring
// points at. Any d-pad/left
// stick direction (held, not only the edge - a held stick is what the user
// feels as "I am using the stick") turns it off again, and R2 cannot arm it on
// that same frame. `turned_on`/`turned_off` are the two transitions the VR host
// logs once each.
//
// Pure and header-only-fed: no pads, no Vulkan, no console (tests/
// ui_input_mode_test.cpp drives it with synthetic action words). The panel's
// own direction bits are pad::ui_actions_for's (pad_input.h, the one input
// map), so the d-pad and the left stick past kUiStickThreshold arrive here as
// the same Up/Down/Left/Right.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "ui/ui_input.h"

namespace splat {

class PointerMode {
public:
    // One frame's decision, read by the VR host before it traces the ray.
    struct Step {
        // The ray is armed this frame: only now may pointer_valid be true.
        bool pointer = false;
        // This R2 press edge is a click (R2 pressed again while already armed).
        bool click = false;
        // R2 is held while armed, the drag level a slider follows. False on the
        // arming frame: that ray position is not a drag start.
        bool down = false;
        // Armed by this frame's R2 edge (log "pointer on").
        bool turned_on = false;
        // Disarmed by a d-pad/stick direction this frame (log "pointer off").
        bool turned_off = false;
    };

    // The four focus directions, d-pad or left stick.
    static constexpr uint32_t kDirections = Up | Down | Left | Right;

    // One frame. `pressed`/`held` are UiAction bits (pressed = went down this
    // frame, held = down now), `r2_down` the frame's R2 level for any pad.
    // The rules, in this order:
    //   1. panel closed            -> off, state reset (every open starts on
    //                                 the stick);
    //   2. any direction down      -> off, and R2 cannot arm on this frame
    //                                 (`pressed` or `held`: a press without a
    //                                 hold is impossible from the pad, but the
    //                                 widgets' belt keys on `pressed`, so this
    //                                 rule matches it);
    //   3. R2 edge while off       -> armed, click = false, down = false (the
    //                                 arming press is not a click and not a
    //                                 drag level);
    //   4. R2 edge while armed     -> click = true (stays armed);
    //   pointer follows the armed state, down = R2 held while armed.
    Step update(bool panel_open, uint32_t pressed, uint32_t held, bool r2_down);

    // Forget everything, including the last R2 level: the next frame with R2
    // down is an edge. The panel-closed rule in update() already drops the arm
    // state every closed frame, so a caller only needs this for an unusual
    // forced stop (e.g. the panel never existed this frame).
    void reset();

    bool armed() const { return armed_; }

private:
    bool armed_ = false;
    bool r2_down_ = false;  // previous frame's R2 level, for the edge
};

}  // namespace splat
