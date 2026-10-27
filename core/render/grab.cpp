// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/grab.h"

#include <algorithm>
#include <cmath>

#include "render/hmd_view.h"

namespace splat {
namespace {

float clampf(float value, float low, float high) { return std::max(low, std::min(high, value)); }

bool finite3(const float v[3]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float distance3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Hamilton product, out = a * b (rotations compose: R(a*b) = R(a) R(b)).
void quat_mul(float out[4], const float a[4], const float b[4]) {
    const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

void quat_conj(float out[4], const float q[4]) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

// Normalises q into out; false when q is not a usable rotation, in which case
// out is the identity. A zero quaternion arrives from an untracked pose.
bool quat_unit(float out[4], const float q[4]) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(length) || !(length > 1.0e-6f)) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return false;
    }
    for (int i = 0; i < 4; ++i) out[i] = q[i] / length;
    return true;
}

// out = R(q) * v for a unit quaternion q.
void quat_rotate(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// A rotation of `angle` about one axis (x, y or z), scene convention (y is
// down, so a positive angle about y is a positive yaw: CameraPose's sign).
void quat_axis(float out[4], int axis, float angle) {
    const float half = 0.5f * angle;
    out[0] = out[1] = out[2] = 0.0f;
    out[axis] = std::sin(half);
    out[3] = std::cos(half);
}

// A hand that can be used: grabbing, and with a finite position and a usable
// orientation. A tracked pose that arrives as zeros or NaNs is not a hand.
bool hand_usable(const HandPose& hand) {
    if (!hand.grabbing || !finite3(hand.position)) return false;
    float unit[4];
    return quat_unit(unit, hand.quaternion);
}

// The angle between two unit quaternions (q and -q are the same rotation).
float quat_angle(const float a[4], const float b[4]) {
    float dot = 0.0f;
    for (int i = 0; i < 4; ++i) dot += a[i] * b[i];
    dot = std::fabs(dot);
    if (!std::isfinite(dot)) return 3.14159265358979f;
    dot = std::min(1.0f, dot);
    return 2.0f * std::acos(dot);
}

// An Hmd2 device orientation in the camera's own axes: Hmd2's local frame is
// +y up and -z forward, the camera's is +y down and +z forward, which is the
// 180 degree rotation about x applied on both sides, i.e. (x, y, z, w) ->
// (-x, y, z, -w). This is the quaternion hmd_basis()/hmd_forward() build a
// matrix from (q and -q are the same rotation).
void tracker_quaternion(const float quaternion[4], float out[4]) {
    out[0] = -quaternion[0];
    out[1] = quaternion[1];
    out[2] = quaternion[2];
    out[3] = -quaternion[3];
}

}  // namespace

void GrabState::reset() { *this = GrabState{}; }

SceneTransform update_grab(GrabState& state, const HandPose& left, const HandPose& right,
                           const SceneTransform& in, double now) {
    SceneTransform out = in;
    // The range is an invariant of the transform, whatever came in.
    out.scale = clampf(in.scale, kMinWorldScale, kMaxWorldScale);
    float unit[4];
    if (!quat_unit(unit, in.rotation)) {
        out.rotation[0] = out.rotation[1] = out.rotation[2] = 0.0f;
        out.rotation[3] = 1.0f;
    }

    const bool left_ok = hand_usable(left);
    const bool right_ok = hand_usable(right);
    if (!left_ok && !right_ok) {
        // Released. A one-hand press that ended inside kTapSeconds with the
        // hand almost still was a tap: undo the grab's own small rigid delta
        // (the hand's net motion and turn since the press) and name the scene
        // step. A longer hold, more motion, or a two-hand gesture keeps the
        // grab's result.
        GrabTap tap = GrabTap::none;
        if (state.tap_target_ != GrabTap::none && state.tap_timed_ && std::isfinite(now) &&
            now >= state.tap_start_ && (now - state.tap_start_) <= kTapSeconds) {
            const HandPose& last = state.tap_target_ == GrabTap::previous ? state.previous_left_
                                                                         : state.previous_right_;
            float start_unit[4];
            float last_unit[4];
            const bool start_ok = quat_unit(start_unit, state.tap_quaternion_);
            const bool last_ok = last.grabbing && finite3(last.position) &&
                                 quat_unit(last_unit, last.quaternion);
            if (start_ok && last_ok && distance3(last.position, state.tap_position_) < kTapMetres &&
                quat_angle(start_unit, last_unit) < kTapRadians) {
                // The grab's net transform over the gesture is a rotation A
                // about the press position plus the hand's translation:
                //   display = hand_now + A * (display_press - hand_press)
                // with A = q_now * q_press^-1. Apply its inverse to what the
                // viewer stores now, so stick motion in the same window is
                // kept and only the grab's delta is undone.
                float start_inverse[4];
                float delta[4];
                quat_conj(start_inverse, start_unit);
                quat_mul(delta, last_unit, start_inverse);
                quat_unit(delta, delta);
                float inverse[4];
                quat_conj(inverse, delta);
                const float relative[3] = {out.offset[0] - last.position[0],
                                           out.offset[1] - last.position[1],
                                           out.offset[2] - last.position[2]};
                float rotated[3];
                quat_rotate(inverse, relative, rotated);
                for (int axis = 0; axis < 3; ++axis) {
                    out.offset[axis] = state.tap_position_[axis] + rotated[axis];
                }
                float rotation[4];
                quat_mul(rotation, inverse, out.rotation);
                quat_unit(out.rotation, rotation);
                tap = state.tap_target_;
            }
        }
        // The transform is now where the scene was before the press (for a
        // tap) or exactly what it was (a hold), and the next grab starts from
        // wherever the scene was left.
        state.reset();
        state.tap_ = tap;
        return out;
    }

    const bool two_handed = left_ok && right_ok;
    const bool hands_changed =
        state.active_ && (two_handed != state.two_handed_ || left_ok != state.left_grabbing_ ||
                          right_ok != state.right_grabbing_);
    if (!state.active_ || hands_changed) {
        // The first frame of a gesture, or a hand joined or left it: seed the
        // reference pose from this frame and move nothing, so taking hold never
        // jumps the scene. Only the first frame can become a tap, and only when
        // exactly one hand pressed; a hand joining later makes it a two-hand
        // gesture for good.
        if (!state.active_) {
            state.tap_target_ =
                two_handed ? GrabTap::none : (left_ok ? GrabTap::previous : GrabTap::next);
            state.tap_timed_ = std::isfinite(now) && now >= 0.0;
            state.tap_start_ = state.tap_timed_ ? now : 0.0;
            const HandPose& press = left_ok ? left : right;
            for (int axis = 0; axis < 3; ++axis) state.tap_position_[axis] = press.position[axis];
            quat_unit(state.tap_quaternion_, press.quaternion);
        } else {
            state.tap_target_ = GrabTap::none;
        }
        state.active_ = true;
        state.two_handed_ = two_handed;
        state.left_grabbing_ = left_ok;
        state.right_grabbing_ = right_ok;
        state.previous_left_ = left;
        state.previous_right_ = right;
        state.start_span_ = two_handed ? distance3(left.position, right.position) : 0.0f;
        state.previous_span_ = state.start_span_;
        for (int axis = 0; axis < 3; ++axis) {
            state.midpoint_[axis] =
                two_handed ? 0.5f * (left.position[axis] + right.position[axis])
                           : (left_ok ? left.position[axis] : right.position[axis]);
        }
        return out;
    }

    if (two_handed) {
        // Scale by how much further apart the hands are than last frame - the
        // product of the frame ratios is the ratio to the gesture's start, and
        // taking it per frame means the clamp (and a stick press's scale) is
        // respected without the scene jumping when the range is reached.
        const float span = distance3(left.position, right.position);
        float ratio = 1.0f;
        if (state.previous_span_ > kMinHandSpan && span > kMinHandSpan) {
            ratio = span / state.previous_span_;
        }
        out.scale = clampf(in.scale * ratio, kMinWorldScale, kMaxWorldScale);
        // The midpoint is the scale's fixed point: the point of the scene that
        // was displayed there is still displayed there after the scale.
        const float applied = in.scale > 0.0f ? out.scale / in.scale : 1.0f;
        float midpoint[3];
        for (int axis = 0; axis < 3; ++axis) {
            midpoint[axis] = 0.5f * (left.position[axis] + right.position[axis]);
            out.offset[axis] = midpoint[axis] + applied * (in.offset[axis] - state.midpoint_[axis]);
            state.midpoint_[axis] = midpoint[axis];
        }
        state.previous_span_ = span;
        // Two hands scale; they do not turn the scene (the brief's two-hand
        // gesture). A wrist turn with both grips down is left to one hand.
    } else {
        const HandPose& now = left_ok ? left : right;
        const HandPose& previous = left_ok ? state.previous_left_ : state.previous_right_;
        // The hand's own frame moved by this rotation and this translation; the
        // scene keeps its offset in that frame, which is a rotation about the
        // hand's previous position followed by the hand's translation.
        float previous_unit[4];
        float now_unit[4];
        quat_unit(previous_unit, previous.quaternion);
        quat_unit(now_unit, now.quaternion);
        float previous_inverse[4];
        float delta[4];
        quat_conj(previous_inverse, previous_unit);
        quat_mul(delta, now_unit, previous_inverse);
        quat_unit(delta, delta);

        float relative[3] = {in.offset[0] - previous.position[0],
                             in.offset[1] - previous.position[1],
                             in.offset[2] - previous.position[2]};
        float rotated[3];
        quat_rotate(delta, relative, rotated);
        for (int axis = 0; axis < 3; ++axis) out.offset[axis] = now.position[axis] + rotated[axis];

        float rotation[4];
        quat_mul(rotation, delta, out.rotation);
        quat_unit(out.rotation, rotation);
    }

    state.two_handed_ = two_handed;
    state.left_grabbing_ = left_ok;
    state.right_grabbing_ = right_ok;
    state.previous_left_ = left;
    state.previous_right_ = right;
    return out;
}

SceneTransform scene_transform(const ViewerAdjust& adjust, const float head[3]) {
    SceneTransform out;
    out.scale = clampf(adjust.scale, kMinWorldScale, kMaxWorldScale);
    if (std::isfinite(adjust.turn)) {
        quat_axis(out.rotation, 1, adjust.turn);
    }
    // The viewer's turn rotates the picture about the eye, so the same picture
    // as a rotation about the scene origin needs the eye-relative offset.
    float relative[3] = {adjust.offset[0] - head[0], adjust.offset[1] - head[1],
                         adjust.offset[2] - head[2]};
    float inverse[4];
    quat_conj(inverse, out.rotation);
    float rotated[3];
    quat_rotate(inverse, relative, rotated);
    for (int axis = 0; axis < 3; ++axis) out.offset[axis] = head[axis] + rotated[axis];
    return out;
}

ViewerAdjust viewer_adjust(const SceneTransform& transform, const float head[3]) {
    ViewerAdjust out;
    out.scale = clampf(transform.scale, kMinWorldScale, kMaxWorldScale);
    float rotation[4];
    quat_unit(rotation, transform.rotation);
    out.turn = quat_turn(rotation);
    // The inverse of scene_transform(): rotate the offset's head-relative part
    // by the turn the viewer will apply about the eye. The yaw itself, not the
    // full rotation: the viewer has no pitch or roll to apply, so the offset
    // has to match the rotation that will actually happen.
    float yaw[4];
    quat_axis(yaw, 1, out.turn);
    float relative[3] = {transform.offset[0] - head[0], transform.offset[1] - head[1],
                         transform.offset[2] - head[2]};
    float rotated[3];
    quat_rotate(yaw, relative, rotated);
    for (int axis = 0; axis < 3; ++axis) out.offset[axis] = head[axis] + rotated[axis];
    return out;
}

float quat_turn(const float quaternion[4]) {
    float q[4];
    quat_unit(q, quaternion);
    // The heading of the rotated forward axis (0, 0, 1): x = 2(xz + wy),
    // z = 1 - 2(x^2 + y^2), the same pair hmd_forward() uses.
    const float x = 2.0f * (q[0] * q[2] + q[3] * q[1]);
    const float z = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
    if (!std::isfinite(x) || !std::isfinite(z)) return 0.0f;
    if (std::fabs(x) < 1.0e-7f && std::fabs(z) < 1.0e-7f) return 0.0f;
    return std::atan2(x, z);
}

HandPose grab_hand_pose(const GrabFrame& frame, const float position[3],
                        const float quaternion[4], bool grabbing) {
    HandPose hand;
    if (!grabbing) return hand;
    if (!finite3(position) || !finite3(frame.head_position) ||
        !finite3(frame.head_scene.position) || !finite3(frame.scene_offset)) {
        return hand;   // untracked: the same as a released hand
    }

    // The head's orientation in the two frames: its tracker pose in the
    // camera's axes, and the pose the renderer uses (the placement frame).
    float head_scene[4];
    tracker_quaternion(frame.head_quaternion, head_scene);
    float head_placement[4];
    {
        float yaw[4];
        float pitch[4];
        float roll[4];
        quat_axis(yaw, 1, frame.head_scene.yaw);
        quat_axis(pitch, 0, frame.head_scene.pitch);
        quat_axis(roll, 2, frame.head_scene.roll);
        // CameraPose's decomposition: R = Ry(yaw) Rx(pitch) Rz(roll).
        float pitched[4];
        quat_mul(pitched, yaw, pitch);
        quat_mul(head_placement, pitched, roll);
        quat_unit(head_placement, head_placement);
    }
    float head_placement_inverse[4];
    quat_conj(head_placement_inverse, head_placement);
    // placement -> scene axes, the rotation the whole tracker frame differs by.
    float placement_to_scene[4];
    quat_mul(placement_to_scene, head_scene, head_placement_inverse);
    quat_unit(placement_to_scene, placement_to_scene);
    float scene_to_placement[4];
    quat_conj(scene_to_placement, placement_to_scene);

    // The head in the placement frame: the camera is the head minus the scene
    // offset (ViewerState), and the offset is in the placement frame.
    float head_placement_position[3];
    for (int axis = 0; axis < 3; ++axis) {
        head_placement_position[axis] = frame.head_scene.position[axis] + frame.scene_offset[axis];
    }
    float head_tracker_position[3];
    hmd_scene_position(frame.head_position, head_tracker_position);
    float hand_tracker_position[3];
    hmd_scene_position(position, hand_tracker_position);

    float relative[3];
    for (int axis = 0; axis < 3; ++axis) {
        relative[axis] = hand_tracker_position[axis] - head_tracker_position[axis];
    }
    float rotated[3];
    quat_rotate(scene_to_placement, relative, rotated);
    for (int axis = 0; axis < 3; ++axis) {
        hand.position[axis] = head_placement_position[axis] + rotated[axis];
    }

    float hand_scene[4];
    tracker_quaternion(quaternion, hand_scene);
    float hand_placement[4];
    quat_mul(hand_placement, scene_to_placement, hand_scene);
    if (!quat_unit(hand.quaternion, hand_placement)) return HandPose{};
    hand.grabbing = true;
    return hand;
}

}  // namespace splat
