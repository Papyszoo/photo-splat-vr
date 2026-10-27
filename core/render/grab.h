// Sense grab: one hand moves and turns the scene, two hands scale it in place
// (docs/ui-v1-plan.md workstream 26).
//
// The user holds a Sense controller's grip (L1 on the left controller, R1 on the
// right - HLA-PSVR2 psvr2.c:386-389, 530) and the scene keeps its place in that
// hand's frame: it follows the hand's translation and its rotation (yaw, pitch
// and roll - a wrist turn is not restricted to one axis), all expressed as the
// same transform the viewer applies. Holding both grips scales the scene by the
// ratio of the hand separation to its value at the start of the gesture, about
// the point between the hands, and the translation follows that point. Releasing
// keeps the result. A press that is released within kTapSeconds with the hand
// almost still is a tap instead: its own tiny grab delta is undone and it steps
// the scene (left grip = previous, right grip = next, the DualSense L1/R1
// mapping). Nothing here reads or writes the head pose, the anchor or
// the comfort radius: the viewer never moves, which is the hard requirement from
// the third headset test (VR5's scale compensation moved the camera by up to
// 139 m, the reported "sends us flying").
//
// The maths is plain arrays and pure functions, so the desktop test drives it
// with scripted hand poses and no headset; the console wiring
// (platform/ps5/viewer/vr_mode.cpp) is UNVERIFIED on hardware. The depth
// measures and the renderer are untouched: a grab only changes the scene
// transform.
//
// The transform model is ViewerState's, extended by the rotation a hand grab
// adds: a scene point x is displayed at `scale * R * x + offset`, and
// scene_transform()/viewer_adjust() translate between it and the two values the
// viewer actually stores (ViewerState's world scale and scene offset, plus
// HmdView's yaw-only turn about the placement frame's vertical). Both are
// exact: a grab that is put through the viewer and read back is the same
// transform.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "render/camera.h"
#include "render/viewer_state.h"

namespace splat {

// The scene as the viewer displays it: p = scale * R * p_scene + offset, with
// `rotation` a unit quaternion (x, y, z, w) and everything in the placement
// frame (the frame ViewerState's scene offset and HmdView's turn live in - the
// head's frame at the last recentre). `scale` is clamped to
// [kMinWorldScale, kMaxWorldScale], the same range L2/R2 use.
struct SceneTransform {
    float scale = 1.0f;
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float offset[3] = {0.0f, 0.0f, 0.0f};
};

// One tracked hand, in the placement frame: position in metres and orientation
// as a unit quaternion (x, y, z, w) in the camera's axes (x right, y down,
// z forward, the same decomposition CameraPose uses). `grabbing` is the grip
// hold: a hand that is not grabbing is ignored and keeps the transform.
struct HandPose {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool grabbing = false;
};

// What a grip tap asked for, if anything. A press always starts a grab at once;
// when it is released inside kTapSeconds with the hand almost still it was a
// tap instead, and the press's own small grab delta is undone. The step is the
// DualSense L1/R1 mapping: the left grip is "previous scene" and the right one
// "next scene" (docs/ui-v1-plan.md section 2). No step is taken when both
// grips were down for the tap: a two-hand gesture is never a tap.
enum class GrabTap { none, previous, next };

// What the previous frame of the gesture was, so a delta can be taken. A
// GrabState belongs to one session's grab handling; update_grab() owns every
// field, callers only ask it questions. Released (no hand grabbing), it is
// empty and update_grab() keeps the result the transform already holds; the one
// exception is a tap, which undoes the press's own small delta and is reported
// once by take_tap().
struct GrabState {
    bool active() const { return active_; }
    // Both hands were grabbing on the previous frame.
    bool two_handed() const { return two_handed_; }
    // Hands positions in the placement frame at the start of this gesture, for
    // the log: the separation the two-hand scale is a ratio of, and where the
    // first hand was.
    float start_span() const { return start_span_; }
    // The tap a release produced, if any. update_grab() records it on the
    // release frame; take_tap() reads and clears it, so one release steps the
    // scene at most once.
    GrabTap take_tap() {
        const GrabTap tap = tap_;
        tap_ = GrabTap::none;
        return tap;
    }
    void reset();

    // update_grab()'s memory. Public like the fields of the plain structs
    // above; nothing outside this file is expected to write them.
    bool active_ = false;
    bool two_handed_ = false;
    bool left_grabbing_ = false;
    bool right_grabbing_ = false;
    HandPose previous_left_;
    HandPose previous_right_;
    // The previous frame's hand midpoint and separation (two hands).
    float midpoint_[3] = {0.0f, 0.0f, 0.0f};
    float previous_span_ = 0.0f;
    float start_span_ = 0.0f;
    // Tap classification: the one hand that pressed, its pose at the press and
    // the clock, so a quick release can be recognised and rolled back. `none`
    // means the gesture was not a one-hand press (both grips, or no clock), so
    // its release is never a tap.
    GrabTap tap_target_ = GrabTap::none;
    bool tap_timed_ = false;
    double tap_start_ = 0.0;
    float tap_position_[3] = {0.0f, 0.0f, 0.0f};
    float tap_quaternion_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // The release frame's tap, consumed by take_tap(). Cleared by reset().
    GrabTap tap_ = GrabTap::none;
};

// Below this separation (metres) two hands are one point: the scale ratio is
// held at 1 instead of dividing by (almost) zero, and the gesture does not
// jump. The same floor applies to the previous frame's separation, so a pair
// that starts coincident and opens up scales from the floor, not from a spike.
inline constexpr float kMinHandSpan = 0.02f;

// A grip press that is released within this window (seconds) is a candidate
// tap; it also has to have stayed still: the hand moved less than kTapMetres
// and turned less than kTapRadians since the press. A longer hold, or more
// motion, stays a grab (the scene followed the hand the whole time).
inline constexpr double kTapSeconds = 0.25;
inline constexpr float kTapMetres = 0.02f;
inline constexpr float kTapRadians = 3.0f * 3.14159265358979f / 180.0f;

// One frame of grab. `in` is the viewer's current scene transform (the sticks
// are applied to it outside, so a stick press while grabbing composes). `now`
// is the gesture clock in seconds (any monotonic origin; `now_loop / 1000` on
// the console). It classifies a release as a tap; pass a negative value when
// there is no clock and releases are always plain grabs. Returns the transform
// the viewer should apply now:
//
//  - one hand: the scene keeps its offset in the hand's frame, so the hand's
//    motion and rotation are applied about the hand's previous position;
//  - two hands: the scale is multiplied by the frame's hand separation ratio
//    (about the start of the gesture when nothing clamps, and clamped to the
//    world-scale range), the rotation is left alone, and the translation
//    follows the midpoint, which is the fixed point of the scale;
//  - the first frame of a gesture, a change of which hands are down, and every
//    frame with no hand grabbing return `in` unchanged, so grabbing and letting
//    go never moves the scene by itself - except a tap: a press released inside
//    kTapSeconds with the hand almost still returns the transform with the
//    press's own tiny grab delta undone, and take_tap() names the scene step.
SceneTransform update_grab(GrabState& state, const HandPose& left, const HandPose& right,
                           const SceneTransform& in, double now = -1.0);

// ---- the viewer's own transform --------------------------------------------
// What ViewerState and HmdView actually store: the world scale, the scene
// offset (placement frame, metres) and HmdView's right-stick turn, a yaw about
// the placement frame's vertical.
struct ViewerAdjust {
    float scale = 1.0f;
    float turn = 0.0f;
    float offset[3] = {0.0f, 0.0f, 0.0f};
};

// Converts the viewer's adjust to the grab's transform. `head` is the head's
// position in the placement frame (the camera position plus the scene offset,
// i.e. the eye midpoint before any scene adjust), because the viewer's turn
// rotates the picture about the eye, not about the scene origin: a turn of t
// with offset o shows the same picture as a rotation of t about the scene
// origin with offset head + R(-t)(o - head).
SceneTransform scene_transform(const ViewerAdjust& adjust, const float head[3]);

// The inverse. A rotation that is not a pure yaw (a hand that pitched or rolled
// the scene) keeps only its heading: the viewer's turn carries no pitch or
// roll, so turning about the vertical is the closest it can display. Exact
// round trip with scene_transform() for a yaw transform.
ViewerAdjust viewer_adjust(const SceneTransform& transform, const float head[3]);

// The turn angle of a rotation about the placement frame's vertical: the
// heading of the rotated forward axis, in the sign turn_scene() takes (positive
// turns the scene to the user's right). Zero for the identity rotation.
float quat_turn(const float quaternion[4]);

// ---- tracked hands ---------------------------------------------------------
// What one frame's hand conversion needs: where the head is, in both frames.
// The hand poses come from hmd2::controller_pose() (W25) in Hmd2's tracker
// space: metres, +y up, -z forward, quaternion (x, y, z, w). The grab works in
// the placement frame, so each hand is carried through the head's own tracker
// -> placement transform.
struct GrabFrame {
    // The head's tracker pose, unconverted (Hmd2's convention; the eye
    // midpoint hmd2 reports for the head).
    float head_position[3] = {0.0f, 0.0f, 0.0f};
    float head_quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // The head's pose in the placement frame, as the renderer has it
    // (FrameView::midpoint). `yaw` must be the head's own yaw
    // (HmdView::scene_yaw()), not the rendered camera yaw, which has the stick
    // turn removed; pitch and roll are the same either way. `position` is the
    // camera, i.e. the head minus the scene offset.
    CameraPose head_scene;
    // The viewer's scene offset, placement frame, metres.
    float scene_offset[3] = {0.0f, 0.0f, 0.0f};
};

// One tracked hand in the placement frame, ready for update_grab(). A hand that
// is not grabbing comes back with `grabbing` false and needs no valid pose.
// Non-finite input is treated as untracked (grabbing false): a bad pose can
// never move the scene.
HandPose grab_hand_pose(const GrabFrame& frame, const float position[3],
                        const float quaternion[4], bool grabbing);

}  // namespace splat
