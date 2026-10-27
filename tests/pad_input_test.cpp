// VR3/VR5/VR7: the viewer's one input table, shared by the DualSense and the
// two PSVR2 Sense controllers (platform/ps5/viewer/pad_input.h).
//
// The table is pure, so it is checked here on the desktop: the buttons by
// meaning (not by position), the sticks and triggers of both layouts mapped to
// the viewer's motion (walk/rise/turn) and the scene's scale, the recentre
// hold's timing (complete at 1 s, cancel before it, a short press does
// nothing), and the per-frame OR of three devices. The console run is what
// proves the reads themselves.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "../platform/ps5/viewer/pad_input.h"
#include "render/viewer_state.h"

TEST_CASE("viewer pad bits are single bits") {
    using namespace viewer::pad;
    const uint32_t bits[] = {kCreate, kL3, kR3, kOptions, kL2, kR2, kL1,
                             kR1,     kTriangle, kCircle, kCross, kSquare};
    uint32_t seen = 0;
    for (const uint32_t bit : bits) {
        CHECK((bit & (bit - 1)) == 0);   // one bit
        CHECK((seen & bit) == 0);        // no two buttons share one
        seen |= bit;
    }
    // The actions are single bits too, so the per-frame OR cannot alias.
    CHECK((kMenu & kTooltips) == 0);
    CHECK((kMenu & kRecentre) == 0);
    CHECK((kMenu & kPrevScene) == 0);
    CHECK((kMenu & kNextScene) == 0);
    CHECK((kTooltips & kRecentre) == 0);
    CHECK((kTooltips & kPrevScene) == 0);
    CHECK((kTooltips & kNextScene) == 0);
    CHECK((kRecentre & kPrevScene) == 0);
    CHECK((kRecentre & kNextScene) == 0);
    CHECK((kPrevScene & kNextScene) == 0);
    // The intercepted flag is not a button bit: it sits above all of them.
    for (const uint32_t bit : bits) CHECK((kIntercepted & bit) == 0);
}

TEST_CASE("viewer DualSense buttons to actions") {
    using namespace viewer::pad;
    // Options opens the menu; it no longer quits.
    CHECK(actions_for(kOptions) == kMenu);
    CHECK((actions_for(kOptions) & kMenu) != 0);
    // Triangle is the tooltips toggle.
    CHECK(actions_for(kTriangle) == kTooltips);
    // Cross is a hold, not an edge action: a press does nothing.
    CHECK(actions_for(kCross) == 0);
    CHECK(actions_for(kR3) == 0);
    CHECK(actions_for(kCross | kR3) == 0);
    CHECK(actions_for(kL1) == kPrevScene);
    CHECK(actions_for(kR1) == kNextScene);
    // Create is the system's screenshot button, not the menu: it does nothing.
    // L3, face buttons, triggers and sticks have no action.
    CHECK(actions_for(kCreate) == 0);
    CHECK(actions_for(kCreate | kL3 | kL2 | kR2 | kSquare | kCircle) == 0);
    // A held combination gives every action it carries.
    CHECK(actions_for(kOptions | kL1) == (kMenu | kPrevScene));
    CHECK(actions_for(kOptions | kTriangle | kR1) == (kMenu | kTooltips | kNextScene));
}

TEST_CASE("viewer Sense controller buttons to actions, by button meaning") {
    using namespace viewer::pad;
    // The left Sense controller's own word: L1 previous, triangle tooltips,
    // Create (its menu key) does nothing, L3/L2/square have no action.
    CHECK(actions_for(kL1) == kPrevScene);
    CHECK(actions_for(kCreate) == 0);
    CHECK(actions_for(kCreate | kL3 | kL2 | kSquare) == 0);
    CHECK(actions_for(kTriangle) == kTooltips);
    // The right Sense controller's own word: R1 next, Options opens the menu;
    // Cross and R3 do nothing on their own, circle and R2 have no action
    // either.
    CHECK(actions_for(kR1) == kNextScene);
    CHECK(actions_for(kOptions) == kMenu);
    CHECK(actions_for(kCross) == 0);
    CHECK(actions_for(kR3) == 0);
    CHECK(actions_for(kCircle | kR2) == 0);
}

TEST_CASE("viewer Create does nothing on every device") {
    using namespace viewer::pad;
    // The DualSense's Create and the left Sense controller's Create are the same
    // bit (0x1) and the same meaning here: no action. Only the menu item quits;
    // Options opens the menu.
    CHECK(actions_for(kCreate) == 0);
    CHECK(actions_for(kCreate | kL1) == kPrevScene);
    CHECK(actions_for(kCreate) == 0);
    CHECK(actions_for(kOptions) == kMenu);
    CHECK((actions_for(kOptions) & kPrevScene) == 0);
}

TEST_CASE("viewer any device can trigger an action") {
    using namespace viewer::pad;
    // The frame's input is the OR of the three devices (vr_mode.cpp).
    const uint32_t quiet = actions_for(0) | actions_for(0) | actions_for(0);
    CHECK(quiet == 0);
    const uint32_t menu = actions_for(0) | actions_for(0) | actions_for(kOptions);
    CHECK(menu == kMenu);
    const uint32_t all = actions_for(kL1) | actions_for(kCreate) | actions_for(kR1);
    CHECK(all == (kPrevScene | kNextScene));
    // The same action from two devices at once stays one bit.
    const uint32_t twice = actions_for(kR1) | actions_for(kR1);
    CHECK(twice == kNextScene);
}

TEST_CASE("viewer stick and trigger bytes decode with a dead zone") {
    using namespace viewer::pad;
    // Centre and both ends.
    CHECK(axis_from_byte(128) == doctest::Approx(0.0f));
    CHECK(stick_x(255) == doctest::Approx(1.0f));
    CHECK(stick_x(0) == doctest::Approx(-1.0f));
    // The raw y byte is down-positive: pushing the stick up (a smaller byte)
    // must be +1, not -1.
    CHECK(stick_y(0) == doctest::Approx(1.0f));
    CHECK(stick_y(255) == doctest::Approx(-1.0f));
    // The dead zone is 0.15 of full travel, and the rest is rescaled so just
    // outside it the axis is small but not zero.
    const int inside = 128 - static_cast<int>(kStickDeadZone * 127.0f) + 2;
    const int outside = 128 - static_cast<int>(kStickDeadZone * 127.0f) - 2;
    CHECK(stick_x(static_cast<uint8_t>(inside)) == doctest::Approx(0.0f));
    // The byte is below centre, so the axis is negative (left) but small.
    CHECK(stick_x(static_cast<uint8_t>(outside)) < 0.0f);
    CHECK(std::fabs(stick_x(static_cast<uint8_t>(outside))) < 0.1f);
    // A byte two thirds out is about (0.66 - 0.15) / 0.85 = 0.6.
    const float expected = (84.0f / 127.0f - kStickDeadZone) / (1.0f - kStickDeadZone);
    CHECK(stick_x(static_cast<uint8_t>(128 + 84)) == doctest::Approx(expected));
    // Triggers rest at zero and reach one; the rest range is removed.
    CHECK(trigger_from_byte(0) == doctest::Approx(0.0f));
    CHECK(trigger_from_byte(static_cast<uint8_t>(kTriggerDeadZone * 255.0f - 1.0f)) ==
          doctest::Approx(0.0f));
    CHECK(trigger_from_byte(255) == doctest::Approx(1.0f));
    CHECK(trigger_from_byte(128) == doctest::Approx((128.0f / 255.0f - kTriggerDeadZone) /
                                                    (1.0f - kTriggerDeadZone)));
}

TEST_CASE("viewer sticks and triggers map to the viewer control") {
    using namespace viewer::pad;
    Axes dualsense;
    Axes left;
    Axes right;

    // Nothing at rest.
    ViewControl idle = view_control(dualsense, left, right);
    CHECK(idle.move_right == 0.0f);
    CHECK(idle.move_forward == 0.0f);
    CHECK(idle.move_up == 0.0f);
    CHECK(idle.turn == 0.0f);
    CHECK(idle.scale == 0.0f);

    // DualSense: left stick walks (forward, right), right stick rises and
    // turns the viewer right, R2 grows and L2 shrinks.
    dualsense.left_x = 0.5f;
    dualsense.left_y = 0.9f;
    dualsense.right_x = -0.4f;
    dualsense.right_y = 0.7f;
    dualsense.r2 = 0.8f;
    ViewControl pad = view_control(dualsense, left, right);
    CHECK(pad.move_right == doctest::Approx(0.5f));
    CHECK(pad.move_forward == doctest::Approx(0.9f));
    CHECK(pad.turn == doctest::Approx(-0.4f));
    CHECK(pad.move_up == doctest::Approx(0.7f));
    CHECK(pad.scale == doctest::Approx(0.8f));
    dualsense.l2 = 0.3f;
    CHECK(view_control(dualsense, left, right).scale == doctest::Approx(0.5f));

    // Sense: the left controller's stick walks, the right controller's stick
    // turns/rises; the left trigger shrinks and the right one grows, so each
    // hand's trigger has the same meaning as the DualSense's.
    dualsense = Axes{};
    left.left_x = -0.6f;
    left.left_y = 0.25f;
    left.l2 = 0.6f;
    right.right_x = 0.8f;
    right.right_y = -0.5f;
    right.r2 = 0.9f;
    ViewControl sense = view_control(dualsense, left, right);
    CHECK(sense.move_right == doctest::Approx(-0.6f));
    CHECK(sense.move_forward == doctest::Approx(0.25f));
    CHECK(sense.turn == doctest::Approx(0.8f));
    CHECK(sense.move_up == doctest::Approx(-0.5f));
    CHECK(sense.scale == doctest::Approx(0.3f));

    // Both devices at once add, and the result never leaves -1..1.
    dualsense.left_x = 0.8f;
    left.left_x = 0.8f;
    dualsense.r2 = 0.9f;
    right.r2 = 0.9f;
    ViewControl both = view_control(dualsense, left, right);
    CHECK(both.move_right == doctest::Approx(1.0f));
    CHECK(both.scale == doctest::Approx(1.0f));

    // The scale factor doubles every kScaleDoubleSeconds at full trigger once
    // the press has eased in, and does nothing at rest: 3 s of a full press is
    // exactly 2x, half a press sqrt(2)x. The ease-in ramps the rate over the
    // first kScaleEaseSeconds, so the first frame of a press moves nothing and
    // a short tap changes almost nothing.
    CHECK(kScaleDoubleSeconds == doctest::Approx(3.0f));
    CHECK(scale_factor(0.0f, 1.5, 10.0) == doctest::Approx(1.0f));
    CHECK(scale_factor(1.0f, 0.0, 10.0) == doctest::Approx(1.0f));
    CHECK(scale_factor(1.0f, static_cast<double>(kScaleDoubleSeconds), 10.0) ==
          doctest::Approx(2.0f));
    CHECK(scale_factor(-1.0f, static_cast<double>(kScaleDoubleSeconds), 10.0) ==
          doctest::Approx(0.5f));
    CHECK(scale_factor(0.5f, static_cast<double>(kScaleDoubleSeconds), 10.0) ==
          doctest::Approx(1.41421356f));
    // Ease-in: no rate at the instant of the press, half the rate a quarter of
    // the way through, full from kScaleEaseSeconds on.
    CHECK(scale_ease(0.0) == doctest::Approx(0.0f));
    CHECK(scale_ease(kScaleEaseSeconds * 0.5) == doctest::Approx(0.5f));
    CHECK(scale_ease(kScaleEaseSeconds) == doctest::Approx(1.0f));
    CHECK(scale_ease(5.0) == doctest::Approx(1.0f));
    CHECK(scale_factor(1.0f, 1.0, 0.0) == doctest::Approx(1.0f));
    CHECK(scale_factor(1.0f, 1.0, kScaleEaseSeconds * 0.5) ==
          doctest::Approx(std::exp2(0.5 / static_cast<double>(kScaleDoubleSeconds))));
    // The eased 0.25 s of a press changes less than the same 0.25 s at full
    // rate, and the range the viewer clamps to is 0.25x-4x (viewer_state.h).
    CHECK(scale_factor(1.0f, 0.25, 0.1) < scale_factor(1.0f, 0.25, 10.0));
    CHECK(splat::kMinWorldScale == doctest::Approx(0.25f));
    CHECK(splat::kMaxWorldScale == doctest::Approx(4.0f));
}

// VR6: moving was "very slow" at 0.5 m/s on a linear stick. The curve is
// quadratic in the (dead-zone-corrected) stick deflection; VR8 raises the
// Settings default to 2.5 m/s and 40 deg/s (the measured walk was 1.45 m/s at
// the old 1.5 m/s cap), so the middle of the stick is still fine and the edge
// is fast.
TEST_CASE("viewer sticks move at the default 2.5 m/s and turn at 40 deg/s") {
    using namespace viewer::pad;
    CHECK(kDefaultMoveMetresPerSecond == doctest::Approx(2.5f));
    CHECK(kDefaultTurnDegreesPerSecond == doctest::Approx(40.0f));
    CHECK(kMoveMetresPerSecond == doctest::Approx(2.5f));
    CHECK(kTurnRadiansPerSecond * 180.0f / 3.14159265358979f == doctest::Approx(40.0f));

    CHECK(move_rate(0.0f) == doctest::Approx(0.0f));
    CHECK(move_rate(1.0f) == doctest::Approx(2.5f));
    CHECK(move_rate(-1.0f) == doctest::Approx(-2.5f));
    // Half a stick is a quarter of the speed (the fine-control half), not half.
    CHECK(move_rate(0.5f) == doctest::Approx(0.625f));
    CHECK(move_rate(-0.5f) == doctest::Approx(-0.625f));
    CHECK(move_rate(2.0f) == doctest::Approx(2.5f));    // clamped
    // The Settings values are the curve's peak, so the same stick at 4 m/s is
    // exactly 1.6x the move at 2.5 m/s.
    CHECK(move_rate(1.0f, 4.0f) == doctest::Approx(4.0f));
    CHECK(move_rate(0.5f, 4.0f) == doctest::Approx(1.0f));
    // Monotonic and symmetric.
    for (float value = 0.0f; value < 1.0f; value += 0.1f) {
        CHECK(move_rate(value) <= move_rate(value + 0.1f));
        CHECK(move_rate(-value) == doctest::Approx(-move_rate(value)));
    }
    const float deg = 3.14159265358979f / 180.0f;
    CHECK(turn_rate(1.0f) == doctest::Approx(40.0f * deg));
    CHECK(turn_rate(0.5f) == doctest::Approx(10.0f * deg));
    CHECK(turn_rate(1.0f, 90.0f) == doctest::Approx(90.0f * deg));
    CHECK(turn_rate(0.0f) == doctest::Approx(0.0f));
    // The dead zone is unchanged and applies before the curve: a stick at the
    // dead-zone edge is at rest, and just past it moves slowly.
    CHECK(axis_from_byte(static_cast<uint8_t>(128 + 0.15 * 127)) == doctest::Approx(0.0f));
    const float just_past = axis_from_byte(static_cast<uint8_t>(128 + 0.25 * 127));
    CHECK(just_past > 0.0f);
    CHECK(move_rate(just_past) < 0.1f);
}

TEST_CASE("viewer locomotion eases in and out with a 150 ms time constant") {
    using namespace viewer::pad;
    LocomotionEase ease;
    // One frame at 60 Hz: 1 - exp(-1/9) = 10.5 % of the target - a gentle
    // start, not a step.
    LocomotionEase::Step step = ease.update(2.5f, 0.0f, 0.0f, 0.0f, 1.0 / 60.0);
    CHECK(step.right == doctest::Approx(2.5f * (1.0 - std::exp(-(1.0 / 60.0) / 0.15))));
    CHECK(step.right < 0.3f);
    // 0.45 s (three time constants) reaches ~95 %.
    for (int i = 0; i < 26; ++i) step = ease.update(2.5f, 0.0f, 0.0f, 0.0f, 1.0 / 60.0);
    CHECK(step.right == doctest::Approx(2.5f).epsilon(0.06));
    // Release: it decays to exactly zero (so "move end" is not logged forever).
    for (int i = 0; i < 120; ++i) step = ease.update(0.0f, 0.0f, 0.0f, 0.0f, 1.0 / 60.0);
    CHECK(step.right == 0.0f);
    CHECK_FALSE(ease.moving());
    // Frame-rate independent: ten 10 ms steps and one 100 ms step reach the
    // same speed within float round-off.
    LocomotionEase a;
    LocomotionEase b;
    for (int i = 0; i < 10; ++i) a.update(1.0f, 0.0f, 0.0f, 0.0f, 0.01);
    b.update(1.0f, 0.0f, 0.0f, 0.0f, 0.1);
    CHECK(a.update(1.0f, 0.0f, 0.0f, 0.0f, 0.0).right ==
          doctest::Approx(b.update(1.0f, 0.0f, 0.0f, 0.0f, 0.0).right));
}

// VR5-fix: a Sense controller has one stick, and which fields it fills depends
// on the hand. The right controller's stick must reach the right-stick actions
// (rise/turn) through view_control(), not be dropped into left_x/left_y where
// nothing reads it.
TEST_CASE("viewer a Sense stick goes to its hand's role") {
    using namespace viewer::pad;
    const uint8_t full_right = 255;   // stick pushed fully right
    const uint8_t full_up = 0;        // raw y is down-positive: 0 is full up
    const uint8_t full_trigger = 255;

    const Axes right = sense_controller_axes(true, full_right, full_up, full_trigger, 0);
    CHECK(right.left_x == 0.0f);
    CHECK(right.left_y == 0.0f);
    CHECK(right.right_x == doctest::Approx(1.0f));
    CHECK(right.right_y == doctest::Approx(1.0f));
    CHECK(right.r2 == doctest::Approx(1.0f));

    const ViewControl from_right = view_control(Axes{}, Axes{}, right);
    CHECK(from_right.turn == doctest::Approx(1.0f));        // +1 = turn the viewer right
    CHECK(from_right.move_up == doctest::Approx(1.0f));     // up rises the viewer
    CHECK(from_right.move_right == 0.0f);                   // the move stick is not touched
    CHECK(from_right.move_forward == 0.0f);
    CHECK(from_right.scale == doctest::Approx(1.0f));       // the right hand's trigger grows

    const Axes left = sense_controller_axes(false, full_right, full_up, full_trigger, 0);
    CHECK(left.left_x == doctest::Approx(1.0f));
    CHECK(left.left_y == doctest::Approx(1.0f));
    CHECK(left.right_x == 0.0f);
    CHECK(left.right_y == 0.0f);
    CHECK(left.l2 == doctest::Approx(1.0f));

    const ViewControl from_left = view_control(Axes{}, left, Axes{});
    CHECK(from_left.move_right == doctest::Approx(1.0f));
    CHECK(from_left.move_forward == doctest::Approx(1.0f));
    CHECK(from_left.turn == 0.0f);
    CHECK(from_left.move_up == 0.0f);
    CHECK(from_left.scale == doctest::Approx(-1.0f));       // the left hand's trigger shrinks

    // The larger of the hand's two trigger bytes is its trigger (HLA-PSVR2).
    const Axes mixed = sense_controller_axes(true, 128, 128, 20, 220);
    CHECK(mixed.r2 == doctest::Approx(trigger_from_byte(220)));
}

TEST_CASE("viewer the panel action map is the UI v1 table") {
    using namespace viewer::pad;
    // D-pad bits keep their meaning, so a script's Up/Down/Left/Right lines and
    // a real pad reach the same actions.
    CHECK(ui_actions_for(kUp) == splat::Up);
    CHECK(ui_actions_for(kDown) == splat::Down);
    CHECK(ui_actions_for(kLeft) == splat::Left);
    CHECK(ui_actions_for(kRight) == splat::Right);
    CHECK(ui_actions_for(kCross) == splat::Select);
    CHECK(ui_actions_for(kCircle) == splat::Back);
    CHECK(ui_actions_for(kL1) == splat::PagePrev);
    CHECK(ui_actions_for(kR1) == splat::PageNext);
    CHECK(ui_actions_for(kSquare) == splat::Secondary);
    CHECK(ui_actions_for(kTriangle) == splat::Tooltips);
    // Options opens the menu now; it is no longer the scene path's quit bit.
    CHECK(ui_actions_for(kOptions) == splat::Menu);
    CHECK((ui_actions_for(kOptions) & splat::Menu) != 0);
    // Triggers, sticks (as buttons), Create and the stick clicks have no action.
    CHECK(ui_actions_for(kCreate | kL2 | kR2 | kL3 | kR3) == 0);
    CHECK(ui_actions_for(0) == 0);
    // Two buttons at once give both actions, and every action bit is distinct.
    CHECK(ui_actions_for(kUp | kCross) == (splat::Up | splat::Select));
    CHECK(ui_actions_for(kL1 | kR1 | kTriangle) ==
          (splat::PagePrev | splat::PageNext | splat::Tooltips));
    const uint32_t all = ui_actions_for(kUp | kDown | kLeft | kRight | kCross | kCircle | kL1 |
                                        kR1 | kSquare | kTriangle | kOptions);
    CHECK(all == (splat::Up | splat::Down | splat::Left | splat::Right | splat::Select |
                  splat::Back | splat::PagePrev | splat::PageNext | splat::Secondary |
                  splat::Tooltips | splat::Menu));
    // The intercepted word (the system's own menu) must be masked by the caller
    // before this map; the map itself only reads the buttons it knows.
    CHECK((ui_actions_for(kIntercepted | kUp) & splat::Up) != 0);
}

TEST_CASE("viewer the panel action map takes the left stick as d-pad") {
    using namespace viewer::pad;
    // Full deflections, the way an input script writes them.
    CHECK(ui_actions_for(0, 0.0f, 1.0f) == splat::Up);
    CHECK(ui_actions_for(0, 0.0f, -1.0f) == splat::Down);
    CHECK(ui_actions_for(0, -1.0f, 0.0f) == splat::Left);
    CHECK(ui_actions_for(0, 1.0f, 0.0f) == splat::Right);
    // Inside the threshold the stick does nothing; just past it the direction
    // appears (the panel is digital, so there is no analogue magnitude).
    CHECK(ui_actions_for(0, 0.4f, -0.4f) == 0);
    CHECK(ui_actions_for(0, 0.5f, 0.0f) == splat::Right);
    CHECK(ui_actions_for(0, 0.0f, -0.5f) == splat::Down);
    // A diagonal gives both directions; a d-pad press and a stick push OR.
    CHECK(ui_actions_for(0, 1.0f, 1.0f) == (splat::Right | splat::Up));
    CHECK(ui_actions_for(kDown, 1.0f, 0.0f) == (splat::Down | splat::Right));
    // A held stick keeps its action held; the pressed edge is the difference
    // of two frames, exactly like the d-pad's (main.cpp derives it).
    const uint32_t held_before = ui_actions_for(0, 0.0f, 0.0f);
    const uint32_t held_now = ui_actions_for(0, 1.0f, 0.0f);
    CHECK((held_now & ~held_before) == splat::Right);
    CHECK((held_now & ~held_now) == 0);
}

TEST_CASE("viewer Cross hold completes at one second and cancels early") {
    using namespace viewer::pad;
    RecentreHold hold;
    CHECK_FALSE(hold.active());

    // A short press: down at 10.0 s, up at 10.3 s. It cancels at 0.3 s of
    // progress and never completes.
    RecentreHold::Step step = hold.update(true, 10.0);
    CHECK(step.completed == false);
    CHECK(step.cancelled == false);
    CHECK(step.progress == doctest::Approx(0.0f));
    CHECK(hold.active());
    step = hold.update(true, 10.3);
    CHECK(step.completed == false);
    CHECK(step.progress == doctest::Approx(0.3f));
    step = hold.update(false, 10.3);
    CHECK(step.completed == false);
    CHECK(step.cancelled == true);
    CHECK(step.progress == doctest::Approx(0.0f));
    CHECK_FALSE(hold.active());
    CHECK_FALSE(hold.fired());
    // Releasing again is silent, and the next press starts from zero.
    step = hold.update(false, 10.4);
    CHECK(step.completed == false);
    CHECK(step.cancelled == false);
    CHECK(step.progress == doctest::Approx(0.0f));

    // A full hold: down at 20.0, still down at 20.999 (0.999 s, no completion),
    // then at 21.0 exactly 1 s completes. Completion fires once, and holding
    // longer neither completes again nor resets progress.
    step = hold.update(true, 20.0);
    CHECK(step.completed == false);
    step = hold.update(true, 20.999);
    CHECK(step.completed == false);
    CHECK(step.progress == doctest::Approx(0.999f));
    CHECK_FALSE(hold.fired());
    step = hold.update(true, 21.0);
    CHECK(step.completed == true);
    CHECK(step.progress == doctest::Approx(1.0f));
    CHECK(hold.fired());
    step = hold.update(true, 22.0);
    CHECK(step.completed == false);
    CHECK(step.progress == doctest::Approx(1.0f));
    CHECK(hold.active());
    // Letting go after the completion is not a cancel, and the state is idle
    // again: the next press needs its own full second.
    step = hold.update(false, 22.1);
    CHECK(step.completed == false);
    CHECK(step.cancelled == false);
    CHECK(step.progress == doctest::Approx(0.0f));
    CHECK_FALSE(hold.active());
    CHECK_FALSE(hold.fired());
    step = hold.update(true, 22.2);
    CHECK(step.completed == false);
    CHECK(step.progress == doctest::Approx(0.0f));
    step = hold.update(true, 22.5);
    CHECK(step.completed == false);
    CHECK(step.progress == doctest::Approx(0.3f));
    step = hold.update(false, 22.5);
    CHECK(step.cancelled == true);
}

// VR5-fix: the CSV's hold_progress column is the hold's 0..1 (the same value
// the HUD bar draws), not the HUD's -1 "no element" marker: a frame without a
// hold logs 0.000, and the frame the 1 s hold completes logs 1.000.
TEST_CASE("viewer the logged hold progress is 0..1, never the HUD marker") {
    using namespace viewer::pad;
    CHECK(logged_hold_progress(-1.0f) == 0.0f);   // nothing held: the HUD marker
    CHECK(logged_hold_progress(0.0f) == 0.0f);    // just pressed
    CHECK(logged_hold_progress(0.42f) == doctest::Approx(0.42f));
    CHECK(logged_hold_progress(1.0f) == doctest::Approx(1.0f));   // completion frame
    CHECK(logged_hold_progress(2.0f) == doctest::Approx(1.0f));   // clamped to the range

    // The row and the bar agree: the completion frame's step value is what the
    // log carries.
    RecentreHold hold;
    hold.update(true, 5.0);
    const RecentreHold::Step step = hold.update(true, 6.0);
    REQUIRE(step.completed);
    CHECK(logged_hold_progress(step.progress) == doctest::Approx(1.0f));
    CHECK(logged_hold_progress(-1.0f) == doctest::Approx(0.0f));
}
