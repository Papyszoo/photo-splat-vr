// The panel's pointer mode (U3): the joystick/d-pad drives the menu and the
// Sense laser is off until R2 arms it; any d-pad/stick direction turns it off
// again. Pure core code, no Vulkan and no PS5 (the same synthetic action words
// the host builds through pad::ui_actions_for drive it here).
//
// The widgets' same-frame belt is in ui_widgets_test.cpp and the ray's maths in
// ui_pointer_test.cpp (unchanged); the VR host itself (vr_mode.cpp: pointer_valid
// only when armed and the ray hits, the two klog lines) needs the console.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>

#include "ui/pointer_mode.h"

namespace {

splat::PointerMode::Step arm(splat::PointerMode& mode) {
    const splat::PointerMode::Step step = mode.update(true, 0, 0, true);
    REQUIRE(step.turned_on);
    REQUIRE(step.pointer);
    return step;
}

}  // namespace

TEST_CASE("ui_input_mode panel closed never arms, even with R2 held") {
    splat::PointerMode mode;
    for (int frame = 0; frame < 4; ++frame) {
        const splat::PointerMode::Step step = mode.update(false, 0, 0, true);
        CHECK_FALSE(step.pointer);
        CHECK_FALSE(step.click);
        CHECK_FALSE(step.down);
        CHECK_FALSE(step.turned_on);
        CHECK_FALSE(mode.armed());
    }
    // An R2 held from before the panel opened is not a press on its first
    // frame: the level is tracked while closed, so there is no edge.
    const splat::PointerMode::Step opened = mode.update(true, 0, 0, true);
    CHECK_FALSE(opened.pointer);
    CHECK_FALSE(opened.turned_on);
    CHECK_FALSE(opened.click);
}

TEST_CASE("ui_input_mode a direction edge turns the armed pointer off") {
    for (uint32_t direction : {splat::Up, splat::Down, splat::Left, splat::Right}) {
        splat::PointerMode mode;
        arm(mode);
        const splat::PointerMode::Step step = mode.update(true, direction, direction, false);
        CHECK(step.turned_off);
        CHECK_FALSE(step.pointer);
        CHECK_FALSE(step.click);
        CHECK_FALSE(step.down);
        CHECK_FALSE(mode.armed());
        // The next frame of the same held direction is no second transition.
        const splat::PointerMode::Step next = mode.update(true, 0, direction, false);
        CHECK_FALSE(next.turned_off);
        CHECK_FALSE(next.pointer);
    }
}

TEST_CASE("ui_input_mode a direction press alone also turns the pointer off") {
    // The widgets' belt keys on `pressed` alone, so the state machine must not
    // be wider in the other direction: a frame that carries only the press edge
    // (held word empty) disarms too.
    for (uint32_t direction : {splat::Up, splat::Down, splat::Left, splat::Right}) {
        splat::PointerMode mode;
        arm(mode);
        const splat::PointerMode::Step step = mode.update(true, direction, 0, false);
        CHECK(step.turned_off);
        CHECK_FALSE(step.pointer);
        CHECK_FALSE(step.click);
        CHECK_FALSE(step.down);
        CHECK_FALSE(mode.armed());
        // And the same frame's press blocks R2 arming: a fresh press while the
        // direction edge is in `pressed` stays off.
        splat::PointerMode blocked;
        const splat::PointerMode::Step blocked_step = blocked.update(true, direction, 0, true);
        CHECK_FALSE(blocked_step.pointer);
        CHECK_FALSE(blocked_step.turned_on);
    }
}

TEST_CASE("ui_input_mode R2 arms the ray and only the next press clicks") {
    splat::PointerMode mode;
    const splat::PointerMode::Step armed = arm(mode);
    CHECK_FALSE(armed.click);  // the arming press is not a click
    // ...and it is not a drag level either: the ray has just appeared at an
    // arbitrary pixel, so the Settings thumbnail slider must not take it.
    CHECK_FALSE(armed.down);

    // Held for more frames: the drag level, still no click.
    for (int frame = 0; frame < 3; ++frame) {
        const splat::PointerMode::Step step = mode.update(true, 0, 0, true);
        CHECK(step.pointer);
        CHECK(step.down);
        CHECK_FALSE(step.click);
        CHECK_FALSE(step.turned_on);
    }

    // Release: armed, not down.
    const splat::PointerMode::Step released = mode.update(true, 0, 0, false);
    CHECK(released.pointer);
    CHECK_FALSE(released.down);
    CHECK_FALSE(released.click);

    // Press again: one click, exactly once.
    const splat::PointerMode::Step click = mode.update(true, 0, 0, true);
    CHECK(click.click);
    CHECK(click.pointer);
    CHECK(click.down);
    CHECK_FALSE(click.turned_on);
    const splat::PointerMode::Step after = mode.update(true, 0, 0, true);
    CHECK_FALSE(after.click);
    CHECK(after.pointer);
}

TEST_CASE("ui_input_mode a held direction disarms without a new edge") {
    // The stick is what the user feels as "I am using the stick": one press
    // edge, then held frames, must not let the pointer take the focus back.
    splat::PointerMode mode;
    arm(mode);
    const splat::PointerMode::Step step = mode.update(true, 0, splat::Down, false);
    CHECK(step.turned_off);
    CHECK_FALSE(step.pointer);
    CHECK_FALSE(step.click);
    CHECK_FALSE(mode.armed());
}

TEST_CASE("ui_input_mode a held stick blocks R2 arming") {
    splat::PointerMode mode;
    splat::PointerMode::Step step = mode.update(true, 0, splat::Right, true);
    CHECK_FALSE(step.pointer);
    CHECK_FALSE(step.turned_on);
    CHECK_FALSE(step.click);

    // Released and pressed again while the stick is still held: still off.
    step = mode.update(true, 0, splat::Right, false);
    CHECK_FALSE(step.pointer);
    step = mode.update(true, 0, splat::Right, true);
    CHECK_FALSE(step.pointer);
    CHECK_FALSE(step.turned_on);

    // Letting the stick go with R2 still down is not an arm either: no edge.
    step = mode.update(true, 0, 0, true);
    CHECK_FALSE(step.pointer);

    // The next R2 press arms, and it is still not a click.
    step = mode.update(true, 0, 0, false);
    CHECK_FALSE(step.pointer);
    CHECK_FALSE(step.turned_on);  // released, so no edge yet
    step = mode.update(true, 0, 0, true);
    CHECK(step.turned_on);
    CHECK_FALSE(step.click);
}

TEST_CASE("ui_input_mode reset and a panel close put it back to off") {
    splat::PointerMode mode;
    arm(mode);
    mode.reset();
    CHECK_FALSE(mode.armed());
    const splat::PointerMode::Step after_reset = mode.update(true, 0, 0, false);
    CHECK_FALSE(after_reset.pointer);
    CHECK_FALSE(after_reset.click);

    // Every open starts on the joystick: an armed panel that closes forgets
    // the arm state, and reopening without R2 stays off.
    arm(mode);
    const splat::PointerMode::Step closed = mode.update(false, 0, 0, false);
    CHECK_FALSE(closed.pointer);
    CHECK_FALSE(closed.turned_off);  // the joystick log is for the stick only
    const splat::PointerMode::Step reopened = mode.update(true, 0, 0, false);
    CHECK_FALSE(reopened.pointer);
    CHECK_FALSE(reopened.turned_on);
}

TEST_CASE("ui_input_mode a disarmed frame carries no pointer, click or drag") {
    // What the host turns into pointer_valid/click/pointer_down: all false, so
    // there is no cursor, no hover and no `pointer hit` line while disarmed.
    splat::PointerMode mode;
    const splat::PointerMode::Step step = mode.update(true, 0, 0, false);
    CHECK_FALSE(step.pointer);
    CHECK_FALSE(step.click);
    CHECK_FALSE(step.down);
    CHECK_FALSE(step.turned_on);
    // R2 pressed while pointing is only a direction-free frame if no stick is
    // down; the arming press still never clicks.
    const splat::PointerMode::Step arming = mode.update(true, 0, 0, true);
    CHECK(arming.pointer);
    CHECK_FALSE(arming.click);
}
