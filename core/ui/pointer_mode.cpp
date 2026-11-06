// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pointer_mode.h"

namespace splat {

PointerMode::Step PointerMode::update(bool panel_open, uint32_t pressed, uint32_t held,
                                      bool r2_down) {
    Step step;
    const bool r2_edge = r2_down && !r2_down_;
    // The level is tracked even while the panel is closed, so an R2 held from
    // before the panel opened is not an edge on its first frame.
    r2_down_ = r2_down;

    // 1. No panel, no pointer. Every open starts on the joystick, which is the
    // user's request; the state is dropped rather than remembered.
    if (!panel_open) {
        armed_ = false;
        return step;
    }

    // 2. "Using the stick" is held, not only the press edge: a held stick
    // produces one edge, and testing edges alone would let the pointer take the
    // focus back between repeats. R2 cannot arm on a frame a direction is down.
    if ((held & kDirections) != 0 || (pressed & kDirections) != 0) {
        step.turned_off = armed_;
        armed_ = false;
        return step;
    }

    // 3./4. R2 arms the ray; the arming press is not a click, the next one is.
    if (r2_edge) {
        if (armed_) {
            step.click = true;
        } else {
            armed_ = true;
            step.turned_on = true;
        }
    }

    step.pointer = armed_;
    // The arming press is not a drag level either: the ray has only just
    // appeared at an arbitrary pixel, so a slider must not take that position
    // as the start of a drag (Settings' thumbnail slider reads pointer_down).
    // The same press on an already armed ray is a genuine click and stays down.
    step.down = armed_ && r2_down && !step.turned_on;
    return step;
}

void PointerMode::reset() {
    armed_ = false;
    r2_down_ = false;
}

}  // namespace splat
