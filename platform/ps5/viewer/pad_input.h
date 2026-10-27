// The VR viewer's input: one button/trigger/stick table, shared by the
// DualSense and the two PSVR2 Sense controllers.
//
// Both devices deliver the buttons as a 32-bit word in the same PS5 layout
// (ScePadData's first word, and scePadVrControllerReadState's state at +0x00;
// HLA-PSVR2 reads both with one bit set, psvr2.c:386-389 and 517-537). What
// differs is which buttons sit on which device: the DualSense has all of them,
// the Sense pair splits them - left: L1, Create, square/triangle, L3; right:
// R1, Options, cross/circle, R3. The table below is written once over button
// meanings, so a device only hands over its word.
//
// The mapping (docs/ui-v1-plan.md section 2):
//   Options (the right Sense controller's menu button)  -> open the menu
//   Triangle                                            -> toggle tooltips
//   Cross, held for kRecentreHoldSeconds                 -> recentre
//   L1 / R1                                             -> previous / next scene (on
//                                                          a Sense: a tap steps, a
//                                                          hold grabs - W26)
//   L2 / R2 (analogue)                                  -> shrink / grow the scene
//   left stick                                          -> walk (forward/back, right/left)
//   right stick up/down, left/right                     -> rise/sink, turn right/left
//   L1 (left Sense) / R1 (right Sense), held            -> grab (W26)
//
// The sticks move the *viewer*: the left stick walks into the scene (the scene
// comes towards you), the right stick up rises and right turns you to your
// right. VR5/VR6 moved the scene instead, which is the opposite of what a stick
// push means to the user ("everything is the opposite of what it should be",
// headset test 4). L2/R2 scale the *scene* about the frozen cone pivot ahead
// of the eyes (Front: nearest surface, Centre: median), so the viewer does not
// move there either.
//
// A short Cross press does nothing (the hold state machine below decides), and
// R3 no longer recentres. Everything else has no action on any device. Create
// in particular is the system's screenshot button: the DualSense and the left
// Sense controller both send it as 0x1, and neither quits the viewer with it.
// Options is the menu button on the DualSense and the right Sense controller;
// quitting is the menu's "Quit" item (UiCommand::Quit) and the quit-watch file,
// not an input edge.
//
// The analogue reads, from the two layouts HLA-PSVR2 reads (psvr2.c:370-379,
// 517-541): ScePadData has buttons at +0x00 and lx, ly, rx, ry, L2, R2 at
// +0x04..+0x09; a Sense controller's 0x68-byte state has buttons at +0x00, its
// stick at +0x04/+0x05 and the analogue triggers at +0x08/+0x09. Stick bytes
// are 0..255 with 128 at rest and **down positive** on y; the trigger bytes
// are 0..255.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ui/pad_bits.h"
#include "ui/ui_input.h"

namespace viewer {
namespace pad {

// The standard PS5 button bits live in core/ui/pad_bits.h, one table for the
// viewer, the panel UI and the script player (HLA-PSVR2's PAD_* enum,
// psvr2.c:386-389). The using-declarations keep the viewer's names working, so
// this file's readers do not change.
using splat::pad::kCircle;
using splat::pad::kCreate;
using splat::pad::kCross;
using splat::pad::kDown;
using splat::pad::kIntercepted;
using splat::pad::kL1;
using splat::pad::kL2;
using splat::pad::kL3;
using splat::pad::kLeft;
using splat::pad::kOptions;
using splat::pad::kR1;
using splat::pad::kR2;
using splat::pad::kR3;
using splat::pad::kRight;
using splat::pad::kSquare;
using splat::pad::kTriangle;
using splat::pad::kUp;

// One bit per action: a frame's input is the OR of every device's actions, so
// two devices pressed at once still do each thing once.
//
// Options opens the panel menu (it no longer quits: docs/ui-v1-plan.md section
// 2); Triangle toggles the tooltip hints. Quit is the menu item and the
// quit-watch file, so there is no quit action bit.
constexpr uint32_t kMenu = 1u << 0;
constexpr uint32_t kTooltips = 1u << 1;
// Not an edge action any more: the viewer sets this bit on the frame the
// Cross hold completes (for the CSV log and the event).
constexpr uint32_t kRecentre = 1u << 2;
constexpr uint32_t kPrevScene = 1u << 3;
constexpr uint32_t kNextScene = 1u << 4;

// The Sense controller's grip, the button the grab holds (W26): L1 on the left
// controller and R1 on the right. HLA-PSVR2 has the bits at psvr2.c:386-389 and
// maps both onto PSVR2_BTN_GRIP at psvr2.c:530 (a Sense state carries L1 0x400
// on the left pad and R1 0x800 on the right, psvr2.c:519-522). A press starts the
// grab at once; a release inside the tap window with the hand still is a tap
// instead and steps the scene (left = previous, right = next, the DualSense
// L1/R1 mapping), so the viewer reads these bits only from the Sense pads and
// drops the plain kPrevScene/kNextScene action bits from them. On a Sense
// controller the panel's ViewScene is still there. The DualSense's L1/R1 stay
// the immediate scene keys.
constexpr uint32_t kGripLeft = kL1;
constexpr uint32_t kGripRight = kR1;

// How long Cross must be held to recentre (HLA-PSVR2's menu hold is the same
// second, psvr2.c:426-443).
constexpr double kRecentreHoldSeconds = 1.0;

// Sticks below this fraction of full deflection are at rest; past it the axis
// is rescaled so full deflection is still 1.0.
constexpr float kStickDeadZone = 0.15f;
// The triggers rest slightly above zero on real hardware; below this they are
// at rest.
constexpr float kTriggerDeadZone = 0.05f;

// Scene control rates. The scale is exponential and doubles every 3 s at full
// deflection (the viewer clamps it to the world-scale range); the first
// kScaleEaseSeconds of a press ease the rate in, so a tap cannot jump the scene.
// Sticks are quadratic: at full deflection the viewer walks
// kDefaultMoveMetresPerSecond (2.5 m/s, the Settings default) and turns
// kDefaultTurnDegreesPerSecond (40 deg/s), at half deflection a quarter of that
// - fine control near the centre, speed at the edge. Both peaks are Settings
// rows (UiState::move_speed/turn_speed), so the viewer passes its own rate to
// move_rate()/turn_rate(); the constants are what a build without a panel uses.
constexpr float kScaleDoubleSeconds = 3.0f;
constexpr double kScaleEaseSeconds = 0.5;
constexpr float kDefaultMoveMetresPerSecond = 2.5f;
constexpr float kDefaultTurnDegreesPerSecond = 40.0f;
constexpr float kMoveMetresPerSecond = kDefaultMoveMetresPerSecond;
constexpr float kTurnDegreesPerSecond = kDefaultTurnDegreesPerSecond;
constexpr float kTurnRadiansPerSecond = kDefaultTurnDegreesPerSecond * 3.14159265358979f / 180.0f;

// The locomotion ease: a first-order lag with a 150 ms time constant on the
// linear and angular velocity, so a small stick change is gentle instead of a
// step. The target speed is the quadratic curve above; the output is a
// *velocity* (m/s, rad/s) that the caller multiplies by the measured frame dt.
// Frame-rate independent by construction (the same lag for the same wall
// time), and it also removes the one-frame motion spike after a hitch: the
// velocity cannot jump to the target in a single frame.
inline constexpr double kLocomotionEaseSeconds = 0.15;

class LocomotionEase {
public:
    struct Step {
        float right = 0.0f;    // m/s
        float forward = 0.0f;  // m/s
        float up = 0.0f;       // m/s
        float turn = 0.0f;     // rad/s, positive turns the viewer right
    };

    Step update(float right_mps, float forward_mps, float up_mps, float turn_rps,
                double seconds) {
        const float target[4] = {right_mps, forward_mps, up_mps, turn_rps};
        if (seconds > 0.0) {
            // exp() rather than a per-frame fraction: the same response at 60,
            // 120 or a stuttering frame rate.
            const float alpha =
                1.0f - static_cast<float>(std::exp(-seconds / kLocomotionEaseSeconds));
            for (int axis = 0; axis < 4; ++axis) {
                velocity_[axis] += (target[axis] - velocity_[axis]) * alpha;
                // A released stick decays for a few frames, then is exactly 0
                // so "move end" is not logged forever on 1e-6 m/s.
                if (target[axis] == 0.0f && std::fabs(velocity_[axis]) < 1.0e-3f) {
                    velocity_[axis] = 0.0f;
                }
            }
        }
        Step step;
        step.right = velocity_[0];
        step.forward = velocity_[1];
        step.up = velocity_[2];
        step.turn = velocity_[3];
        return step;
    }

    // True while any axis still moves (the caller uses it to end the
    // "move start/end" event only when the motion has really stopped).
    bool moving() const {
        for (float value : velocity_) {
            if (value != 0.0f) return true;
        }
        return false;
    }

    void reset() {
        for (float& value : velocity_) value = 0.0f;
    }

private:
    float velocity_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Which device a button word came from. The action table below ignores it; the
// viewer only uses it to pick the read call and to name the device in its log.
enum class Device { dualsense, sense_left, sense_right };

// The action table. Written once, over button meanings; nothing here knows
// which device it is looking at. Cross is absent on purpose: a press is not an
// action, a one-second hold is (RecentreHold below).
inline uint32_t actions_for(uint32_t buttons) {
    uint32_t actions = 0;
    if ((buttons & kOptions) != 0) actions |= kMenu;
    if ((buttons & kTriangle) != 0) actions |= kTooltips;
    if ((buttons & kL1) != 0) actions |= kPrevScene;
    if ((buttons & kR1) != 0) actions |= kNextScene;
    return actions;
}

// ---- the panel UI's action word --------------------------------------------
// The UI v1 input map, over button meanings (docs/ui-v1-plan.md section 2):
// d-pad / left stick -> Up Down Left Right, Cross -> Select, Circle -> Back,
// L1/R1 -> PagePrev/PageNext, Square -> Secondary, Triangle -> Tooltips,
// Options -> Menu. It is the same table for a real pad and a script's button
// word (core/ui/input_script.h), so a scripted run exercises the real path.
//
// The left stick is digital for the panel: pushing past kUiStickThreshold adds
// the matching d-pad bit. `left_x` is right-positive and `left_y` up-positive,
// the same as Axes above (a script's stick line uses the same convention).
constexpr float kUiStickThreshold = 0.5f;

inline uint32_t ui_actions_for(uint32_t buttons, float left_x = 0.0f, float left_y = 0.0f) {
    uint32_t word = buttons;
    if (left_x <= -kUiStickThreshold) word |= kLeft;
    if (left_x >= kUiStickThreshold) word |= kRight;
    if (left_y >= kUiStickThreshold) word |= kUp;
    if (left_y <= -kUiStickThreshold) word |= kDown;

    uint32_t actions = 0;
    if ((word & kUp) != 0) actions |= splat::Up;
    if ((word & kDown) != 0) actions |= splat::Down;
    if ((word & kLeft) != 0) actions |= splat::Left;
    if ((word & kRight) != 0) actions |= splat::Right;
    if ((word & kCross) != 0) actions |= splat::Select;
    if ((word & kCircle) != 0) actions |= splat::Back;
    if ((word & kL1) != 0) actions |= splat::PagePrev;
    if ((word & kR1) != 0) actions |= splat::PageNext;
    if ((word & kSquare) != 0) actions |= splat::Secondary;
    if ((word & kTriangle) != 0) actions |= splat::Tooltips;
    if ((word & kOptions) != 0) actions |= splat::Menu;
    return actions;
}

// ---- analogue -------------------------------------------------------------

// One stick byte to -1..+1, with the dead zone removed and the rest rescaled.
// 128 is the centre; both ends reach exactly -1 and +1.
inline float axis_from_byte(uint8_t raw) {
    float value = (static_cast<float>(raw) - 128.0f) / 127.0f;
    value = std::max(-1.0f, std::min(1.0f, value));
    const float magnitude = std::fabs(value);
    if (magnitude <= kStickDeadZone) return 0.0f;
    const float rescaled = (magnitude - kStickDeadZone) / (1.0f - kStickDeadZone);
    return value < 0.0f ? -rescaled : rescaled;
}

// x: right positive. y: the raw byte is down positive, so this returns up
// positive (a stick pushed forward is +1).
inline float stick_x(uint8_t raw) { return axis_from_byte(raw); }
inline float stick_y(uint8_t raw) { return -axis_from_byte(raw); }

// One trigger byte to 0..1, with its rest range removed.
inline float trigger_from_byte(uint8_t raw) {
    float value = static_cast<float>(raw) / 255.0f;
    if (value <= kTriggerDeadZone) return 0.0f;
    value = (value - kTriggerDeadZone) / (1.0f - kTriggerDeadZone);
    return std::min(1.0f, value);
}

// One device's analogue axes, already decoded from its raw bytes. The DualSense
// fills all of them; a Sense controller fills its own stick and triggers.
struct Axes {
    float left_x = 0.0f;   // -1 left .. +1 right
    float left_y = 0.0f;   // -1 back .. +1 forward (up)
    float right_x = 0.0f;
    float right_y = 0.0f;
    float l2 = 0.0f;       // 0..1
    float r2 = 0.0f;       // 0..1
};

// One Sense controller's state bytes as that hand's Axes. A Sense controller has
// a single stick, and its role depends on the hand: the left controller's stick
// walks the viewer (left_x/left_y), the right controller's turns and rises it
// (right_x/right_y) - the fields view_control() reads by role. Putting both
// hands' sticks in left_x/left_y would silently drop the right stick, so the
// hand has to be passed in. The hand's trigger lands in both l2 and r2 (the
// left hand's shrinks, the right hand's grows, exactly like the DualSense's two
// triggers); HLA-PSVR2 reads a Sense trigger as the larger of its two bytes
// (psvr2.c:570-573) because the side's byte has varied.
inline Axes sense_controller_axes(bool right_hand, uint8_t stick_x_byte, uint8_t stick_y_byte,
                                  uint8_t trigger_a, uint8_t trigger_b) {
    Axes axes;
    if (right_hand) {
        axes.right_x = stick_x(stick_x_byte);
        axes.right_y = stick_y(stick_y_byte);
    } else {
        axes.left_x = stick_x(stick_x_byte);
        axes.left_y = stick_y(stick_y_byte);
    }
    axes.l2 = axes.r2 = std::max(trigger_from_byte(trigger_a), trigger_from_byte(trigger_b));
    return axes;
}

// What one frame's sticks and triggers ask the viewer to do, in normalised
// units. The viewer turns each into metres, radians or a scale factor with the
// rates above; keeping the mapping here is what the desktop test checks.
struct ViewControl {
    float move_right = 0.0f;    // -1 .. +1, the viewer walks right (left stick)
    float move_forward = 0.0f;  // -1 .. +1, the viewer walks forward (left stick)
    float move_up = 0.0f;       // -1 .. +1, the viewer rises (right stick)
    float turn = 0.0f;          // +1 turns the viewer to their right (right stick)
    float scale = 0.0f;         // -1 shrink (L2) .. +1 grow (R2)
};

inline float clamp_unit(float value) { return std::max(-1.0f, std::min(1.0f, value)); }

// Combines the three pads by role: the DualSense's left stick and the left
// Sense controller's stick walk the viewer; the DualSense's right stick and the
// right Sense controller's stick rise/sink and turn them; L2/R2 and the two
// Sense triggers scale the scene (the left controller's trigger shrinks, the
// right one's grows, so the pair works like the DualSense's two triggers).
inline ViewControl view_control(const Axes& dualsense, const Axes& left, const Axes& right) {
    ViewControl control;
    control.move_right = clamp_unit(dualsense.left_x + left.left_x);
    control.move_forward = clamp_unit(dualsense.left_y + left.left_y);
    control.move_up = clamp_unit(dualsense.right_y + right.right_y);
    control.turn = clamp_unit(dualsense.right_x + right.right_x);
    control.scale = clamp_unit(dualsense.r2 - dualsense.l2 + right.r2 - left.l2);
    return control;
}

// One frame's scale factor for `input` (-1..1): exponential, x2 every
// kScaleDoubleSeconds at full deflection once the press has eased in.
// `held_seconds` is how long the trigger has been down this press: the rate is
// scaled by a smoothstep over kScaleEaseSeconds, so the first frames of a press
// move the scene gently and a short tap changes almost nothing.
inline float scale_ease(double held_seconds) {
    if (!(held_seconds > 0.0)) return 0.0f;
    const double t = std::min(1.0, held_seconds / kScaleEaseSeconds);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

inline float scale_factor(float input, double seconds, double held_seconds) {
    if (input == 0.0f || !(seconds > 0.0)) return 1.0f;
    const double rate = static_cast<double>(input) * static_cast<double>(scale_ease(held_seconds)) *
                        seconds / static_cast<double>(kScaleDoubleSeconds);
    return static_cast<float>(std::exp2(rate));
}

// The speed a stick deflection asks for, m/s: quadratic, so half a stick is a
// quarter of `metres_per_second` and the last few degrees are the fast ones.
// Signed, so a reversal is symmetric. The dead zone is already removed by
// axis_from_byte().
inline float move_rate(float input, float metres_per_second = kDefaultMoveMetresPerSecond) {
    const float value = clamp_unit(input);
    return (value < 0.0f ? -value * value : value * value) * metres_per_second;
}

// The turn a stick deflection asks for, radians/s: the same curve and sign as
// move_rate (positive turns the scene to the user's right).
inline float turn_rate(float input, float degrees_per_second = kDefaultTurnDegreesPerSecond) {
    const float value = clamp_unit(input);
    const float radians = degrees_per_second * 3.14159265358979f / 180.0f;
    return (value < 0.0f ? -value * value : value * value) * radians;
}

// ---- the recentre hold ----------------------------------------------------

// Cross held for kRecentreHoldSeconds recentres; released early it cancels and
// does nothing. The state machine is time-driven and pure, so the console and
// the desktop test run the same code. `now` is seconds on any monotonic clock.
class RecentreHold {
public:
    struct Step {
        bool completed = false;   // this call reached the hold time
        bool cancelled = false;   // released before the hold time
        double held_seconds = 0.0;
        float progress = 0.0f;    // 0..1 while held, 0 when not
    };

    Step update(bool down, double now_seconds) {
        Step step;
        if (down) {
            if (!active_) {
                active_ = true;
                fired_ = false;
                started_ = now_seconds;
            }
            const double held = std::max(0.0, now_seconds - started_);
            step.held_seconds = held;
            progress_ = static_cast<float>(std::min(1.0, held / kRecentreHoldSeconds));
            if (!fired_ && progress_ >= 1.0f) {
                fired_ = true;
                step.completed = true;
            }
        } else if (active_) {
            step.held_seconds = std::max(0.0, now_seconds - started_);
            step.cancelled = !fired_;
            active_ = false;
            fired_ = false;
            progress_ = 0.0f;
        }
        step.progress = progress_;
        return step;
    }

    bool active() const { return active_; }
    bool fired() const { return fired_; }
    float progress() const { return progress_; }

private:
    bool active_ = false;
    bool fired_ = false;
    double started_ = 0.0;
    float progress_ = 0.0f;
};

// The hold progress as the CSV's `hold_progress` column logs it: 0..1 while
// Cross is held (1.0 on the frame the hold completes), 0 when it is not. The
// viewer's HUD uses a negative value as "draw no hold element", which is a
// renderer detail and must never reach the log (the runbook's column is 0.00 to
// 1.00, not -1.00). Same value on both sides, so the bar and the row agree.
inline float logged_hold_progress(float hud_progress) {
    return hud_progress > 0.0f ? std::min(1.0f, hud_progress) : 0.0f;
}

}  // namespace pad
}  // namespace viewer
