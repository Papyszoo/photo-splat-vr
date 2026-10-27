// W26: the Sense grab's maths, on the desktop.
//
// The grab is the one part of the VR input that cannot be seen on the TV: it
// runs on tracked controller poses and the console wiring is UNVERIFIED on
// hardware. These tests drive core/render/grab.* with scripted hand poses and
// check the properties the brief names: one hand translates and rotates the
// scene with it, two hands scale it about the point between them, the scale is
// clamped, a point at the scale's pivot keeps its place in the picture, letting
// go and taking hold again does not jump the scene, and coincident or untracked
// hands do nothing.
//
// The transform model is checked both directly and through the mapping to what
// the viewer stores (ViewerState's world scale and scene offset plus HmdView's
// yaw-only turn): a grab that is converted to the viewer and back is the same
// transform, which is what "share one transform" means.
//
// The W26 fix adds the tap: a press starts the grab at once, and a release
// inside 250 ms with the hand almost still undoes the press's own grab delta
// and steps the scene (left = previous, right = next). Those cases, the shared
// tracker-position mapping (grab.cpp uses hmd_view.h's) and the scale-gesture
// contract the grab end relies on are tested at the end.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>

#include "render/grab.h"
#include "render/hmd_view.h"

namespace {

using splat::GrabFrame;
using splat::GrabState;
using splat::GrabTap;
using splat::HandPose;
using splat::SceneTransform;
using splat::ViewerAdjust;

constexpr float kPi = 3.14159265358979f;

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

void quat_axis(float out[4], int axis, float angle) {
    out[0] = out[1] = out[2] = 0.0f;
    out[axis] = std::sin(0.5f * angle);
    out[3] = std::cos(0.5f * angle);
}

void rotate(const float q[4], const float v[3], float out[3]) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// The displayed position of a scene point under `transform`.
void displayed(const SceneTransform& transform, const float point[3], float out[3]) {
    float rotated[3];
    rotate(transform.rotation, point, rotated);
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = transform.scale * rotated[axis] + transform.offset[axis];
    }
}

HandPose hand(const float x, const float y, const float z, const float q[4] = nullptr,
              bool grabbing = true) {
    HandPose pose;
    pose.position[0] = x;
    pose.position[1] = y;
    pose.position[2] = z;
    if (q != nullptr) {
        for (int i = 0; i < 4; ++i) pose.quaternion[i] = q[i];
    }
    pose.grabbing = grabbing;
    return pose;
}

SceneTransform transform_of(float scale, const float q[4], const float o[3]) {
    SceneTransform transform;
    transform.scale = scale;
    for (int i = 0; i < 4; ++i) transform.rotation[i] = q[i];
    for (int axis = 0; axis < 3; ++axis) transform.offset[axis] = o[axis];
    return transform;
}

SceneTransform transform_at(float scale, const float q[4], float x, float y, float z) {
    const float offset[3] = {x, y, z};
    return transform_of(scale, q, offset);
}

void check_identity(const float quaternion[4]) {
    CHECK(quaternion[0] == doctest::Approx(0.0f).scale(1.0));
    CHECK(quaternion[1] == doctest::Approx(0.0f).scale(1.0));
    CHECK(quaternion[2] == doctest::Approx(0.0f).scale(1.0));
    CHECK(std::fabs(quaternion[3]) == doctest::Approx(1.0f));
}

SceneTransform identity_transform() {
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float o[3] = {0.1f, 0.2f, 3.0f};
    return transform_of(2.0f, q, o);
}

void check_near(const float* actual, const float* expected, int count, float tolerance = 1.0e-5f) {
    for (int i = 0; i < count; ++i) {
        INFO("component " << i << ": " << actual[i] << " vs " << expected[i]);
        CHECK(actual[i] == doctest::Approx(expected[i]).epsilon(tolerance));
    }
}

}  // namespace

TEST_CASE("grab one hand translates the scene with the hand") {
    GrabState state;
    const SceneTransform in = identity_transform();
    // The first frame takes hold: nothing moves.
    const SceneTransform held = splat::update_grab(state, hand(0.1f, 0.2f, 0.3f), HandPose{}, in);
    check_near(held.offset, in.offset, 3);
    CHECK(held.scale == doctest::Approx(in.scale));
    CHECK(state.active());
    CHECK_FALSE(state.two_handed());

    const SceneTransform moved =
        splat::update_grab(state, hand(0.6f, 0.2f, 0.1f), HandPose{}, held);
    const float expected[3] = {in.offset[0] + 0.5f, in.offset[1], in.offset[2] - 0.2f};
    check_near(moved.offset, expected, 3);
    CHECK(moved.scale == doctest::Approx(in.scale));
    check_near(moved.rotation, in.rotation, 4);
}

TEST_CASE("grab one hand rotates the scene about the hand") {
    GrabState state;
    const SceneTransform in = identity_transform();
    const float q0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float q1[4];
    quat_axis(q1, 1, 0.5f * kPi);   // a quarter turn of the wrist (yaw, about down)
    splat::update_grab(state, hand(0.3f, 0.0f, 1.0f, q0), HandPose{}, in);
    const SceneTransform turned =
        splat::update_grab(state, hand(0.3f, 0.0f, 1.0f, q1), HandPose{}, in);

    // The point the hand was holding keeps its place: with the hand still, the
    // scene turns about it.
    const float hand_point[3] = {0.3f, 0.0f, 1.0f};
    float scene_point[3];
    for (int axis = 0; axis < 3; ++axis) {
        scene_point[axis] = (hand_point[axis] - in.offset[axis]) / in.scale;
    }
    float before[3];
    float after[3];
    displayed(in, scene_point, before);
    displayed(turned, scene_point, after);
    check_near(before, hand_point, 3);
    check_near(after, hand_point, 3);
    CHECK(turned.scale == doctest::Approx(in.scale));
    CHECK(splat::quat_turn(turned.rotation) == doctest::Approx(0.5f * kPi).epsilon(1.0e-4));
}

TEST_CASE("grab one hand carries pitch and roll, not only yaw") {
    GrabState state;
    const SceneTransform in = identity_transform();
    const float origin[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float tilted[4];
    quat_axis(tilted, 0, 0.4f);   // pitch about the hand's x axis
    splat::update_grab(state, hand(0.3f, 0.0f, 1.0f, origin), HandPose{}, in);
    const SceneTransform out =
        splat::update_grab(state, hand(0.3f, 0.0f, 1.0f, tilted), HandPose{}, in);

    // offset' = hand + R * (offset - hand), with R the hand's own rotation:
    // nothing gimbal-locks the pitch away.
    const float hand_point[3] = {0.3f, 0.0f, 1.0f};
    const float relative[3] = {in.offset[0] - hand_point[0], in.offset[1] - hand_point[1],
                               in.offset[2] - hand_point[2]};
    float rotated[3];
    rotate(tilted, relative, rotated);
    const float expected[3] = {hand_point[0] + rotated[0], hand_point[1] + rotated[1],
                               hand_point[2] + rotated[2]};
    check_near(out.offset, expected, 3);
    // The composed rotation is the hand's: the scene's forward axis pitched with it.
    const float forward[3] = {0.0f, 0.0f, 1.0f};
    float turned_forward[3];
    rotate(out.rotation, forward, turned_forward);
    CHECK(std::fabs(turned_forward[1]) == doctest::Approx(std::sin(0.4f)).epsilon(1.0e-4));
}

TEST_CASE("grab one hand round trip returns the transform") {
    GrabState state;
    const float start_q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float o[3] = {0.2f, 0.1f, 2.0f};
    const SceneTransform in = transform_of(1.3f, start_q, o);
    const float hand_start[3] = {0.1f, -0.2f, 0.7f};
    splat::update_grab(state, hand(hand_start[0], hand_start[1], hand_start[2]), HandPose{}, in);

    float moved_q[4];
    float yaw[4];
    float pitch[4];
    quat_axis(yaw, 1, 0.6f);
    quat_axis(pitch, 0, 0.2f);
    quat_mul(moved_q, yaw, pitch);
    const SceneTransform moved =
        splat::update_grab(state, hand(0.4f, -0.1f, 0.5f, moved_q), HandPose{}, in);
    const SceneTransform back = splat::update_grab(
        state, hand(hand_start[0], hand_start[1], hand_start[2], start_q), HandPose{}, moved);
    check_near(back.offset, in.offset, 3, 1.0e-4f);
    check_near(back.rotation, in.rotation, 4, 1.0e-4f);
    CHECK(back.scale == doctest::Approx(in.scale));
}

TEST_CASE("grab two hands scale about the point between them") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float o[3] = {0.1f, 0.0f, 2.0f};
    const SceneTransform in = transform_of(1.0f, q, o);
    // Hands 0.6 m apart, midpoint (0, 0, 1).
    splat::update_grab(state, hand(-0.3f, 0.0f, 1.0f), hand(0.3f, 0.0f, 1.0f), in);
    CHECK(state.two_handed());
    CHECK(state.start_span() == doctest::Approx(0.6f));

    // Apart by 2x: the scene doubles, the midpoint stays put.
    const SceneTransform out =
        splat::update_grab(state, hand(-0.6f, 0.0f, 1.0f), hand(0.6f, 0.0f, 1.0f), in);
    CHECK(out.scale == doctest::Approx(2.0f));
    check_near(out.rotation, in.rotation, 4);

    // The scene point that was displayed at the midpoint is still displayed
    // there: that is "scale in place", not a zoom about the eye.
    const float midpoint[3] = {0.0f, 0.0f, 1.0f};
    float scene_point[3];
    for (int axis = 0; axis < 3; ++axis) {
        scene_point[axis] = (midpoint[axis] - in.offset[axis]) / in.scale;
    }
    float shown[3];
    displayed(out, scene_point, shown);
    check_near(shown, midpoint, 3);
    // The viewer's own position is not part of this transform at all: the head,
    // the anchor and the comfort radius stay whatever they were.
}

TEST_CASE("grab two hand scale is clamped to the world scale range") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float o[3] = {0.0f, 0.0f, 2.0f};
    SceneTransform in = transform_of(splat::kMaxWorldScale, q, o);
    splat::update_grab(state, hand(-0.3f, 0.0f, 1.0f), hand(0.3f, 0.0f, 1.0f), in);
    // Ten times further apart: the scene stops at the range's end instead of
    // leaving the world.
    const SceneTransform out =
        splat::update_grab(state, hand(-3.0f, 0.0f, 1.0f), hand(3.0f, 0.0f, 1.0f), in);
    CHECK(out.scale == doctest::Approx(splat::kMaxWorldScale));
    // And a shrink cannot go below the floor (hands 4 cm apart: still a real
    // two-hand span, but far enough from the floor that the range clamps).
    in = out;
    const SceneTransform tiny =
        splat::update_grab(state, hand(-0.02f, 0.0f, 1.0f), hand(0.02f, 0.0f, 1.0f), in);
    CHECK(tiny.scale == doctest::Approx(splat::kMinWorldScale));
    CHECK(std::isfinite(tiny.offset[0]));
    CHECK(std::isfinite(tiny.offset[2]));
}

TEST_CASE("grab release keeps the result and regrab does not jump") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);
    const SceneTransform held = splat::update_grab(state, hand(0.2f, 0.0f, 1.0f), HandPose{}, in);
    const SceneTransform moved =
        splat::update_grab(state, hand(0.5f, 0.1f, 1.0f), HandPose{}, held);
    REQUIRE(moved.offset[0] != doctest::Approx(in.offset[0]));

    // Letting go: the transform is exactly what it was.
    const SceneTransform released = splat::update_grab(state, HandPose{}, HandPose{}, moved);
    check_near(released.offset, moved.offset, 3);
    CHECK(released.scale == doctest::Approx(moved.scale));
    check_near(released.rotation, moved.rotation, 4);
    CHECK_FALSE(state.active());

    // Taking hold again at a new place: the first frame moves nothing, so the
    // scene does not jump to the hand.
    const SceneTransform again =
        splat::update_grab(state, hand(-0.4f, 0.3f, 0.9f), HandPose{}, released);
    check_near(again.offset, moved.offset, 3);
    CHECK(state.active());
    // ... and from there it follows the hand again.
    const SceneTransform after =
        splat::update_grab(state, hand(-0.1f, 0.3f, 0.9f), HandPose{}, again);
    CHECK(after.offset[0] - again.offset[0] == doctest::Approx(0.3f).epsilon(1.0e-4));
}

TEST_CASE("grab hand changes and degenerate poses are safe") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);
    // One hand, then the second joins: the join itself must not move anything.
    const SceneTransform one = splat::update_grab(state, hand(0.2f, 0.0f, 1.0f), HandPose{}, in);
    const SceneTransform two =
        splat::update_grab(state, hand(0.2f, 0.0f, 1.0f), hand(0.8f, 0.0f, 1.0f), one);
    check_near(two.offset, one.offset, 3);
    CHECK(two.scale == doctest::Approx(one.scale));
    CHECK(state.two_handed());

    // Coincident hands: no divide by zero, no NaN, no scale change.
    const SceneTransform same =
        splat::update_grab(state, hand(0.5f, 0.0f, 1.0f), hand(0.5f, 0.0f, 1.0f), two);
    CHECK(same.scale == doctest::Approx(two.scale));
    for (int axis = 0; axis < 3; ++axis) CHECK(std::isfinite(same.offset[axis]));
    for (int i = 0; i < 4; ++i) CHECK(std::isfinite(same.rotation[i]));

    // A hand back at its previous position: exactly the same transform.
    const SceneTransform still =
        splat::update_grab(state, hand(0.5f, 0.0f, 1.0f), hand(0.5f, 0.0f, 1.0f), same);
    check_near(still.offset, same.offset, 3);

    // Zero/NaN poses are untracked, not states of the world.
    HandPose broken = hand(0.0f, 0.0f, 0.0f);
    broken.position[1] = std::nanf("");
    const SceneTransform guarded = splat::update_grab(state, broken, HandPose{}, still);
    check_near(guarded.offset, still.offset, 3);
    CHECK_FALSE(state.active());
}

TEST_CASE("grab viewer adjust round trip is the same transform") {
    ViewerAdjust adjust;
    adjust.scale = 1.5f;
    adjust.turn = 0.7f;
    adjust.offset[0] = 0.3f;
    adjust.offset[1] = -0.2f;
    adjust.offset[2] = 1.1f;
    const float head[3] = {0.1f, 0.0f, 0.4f};

    const SceneTransform transform = splat::scene_transform(adjust, head);
    CHECK(splat::quat_turn(transform.rotation) == doctest::Approx(adjust.turn));
    CHECK(transform.scale == doctest::Approx(adjust.scale));

    const ViewerAdjust back = splat::viewer_adjust(transform, head);
    CHECK(back.scale == doctest::Approx(adjust.scale));
    CHECK(back.turn == doctest::Approx(adjust.turn));
    check_near(back.offset, adjust.offset, 3, 1.0e-5f);

    // No turn, no offset change: the transform is the viewer's offset, which is
    // the one case with an obvious answer.
    ViewerAdjust plain;
    plain.offset[2] = 2.0f;
    const SceneTransform unturned = splat::scene_transform(plain, head);
    check_near(unturned.offset, plain.offset, 3);
    check_identity(unturned.rotation);

    // A hand that pitched the scene keeps only its heading for the viewer: the
    // viewer stores a yaw turn, not a full orientation.
    float yaw[4];
    float pitch[4];
    float full[4];
    quat_axis(yaw, 1, 0.4f);
    quat_axis(pitch, 0, 0.3f);
    quat_mul(full, yaw, pitch);
    SceneTransform tilted = transform;
    for (int i = 0; i < 4; ++i) tilted.rotation[i] = full[i];
    const ViewerAdjust flattened = splat::viewer_adjust(tilted, head);
    CHECK(flattened.turn == doctest::Approx(splat::quat_turn(full)));
    CHECK(std::isfinite(flattened.offset[0]));
}

TEST_CASE("grab hand pose maps a tracked controller into the placement frame") {
    GrabFrame frame;
    // Head at the tracker's origin, level, and its frame is the placement
    // frame's frame: the two frames coincide.
    frame.head_scene.position[0] = 0.0f;
    frame.head_scene.position[1] = 0.0f;
    frame.head_scene.position[2] = 0.0f;

    // Hmd2 tracker space (+y up, -z forward) to the scene's axes.
    const float at[3] = {0.5f, 0.2f, -1.0f};
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const HandPose mapped = splat::grab_hand_pose(frame, at, identity, true);
    CHECK(mapped.grabbing);
    CHECK(mapped.position[0] == doctest::Approx(0.5f));
    CHECK(mapped.position[1] == doctest::Approx(-0.2f));
    CHECK(mapped.position[2] == doctest::Approx(1.0f));
    // An identity device orientation is the identity in the camera's axes.
    CHECK(std::fabs(mapped.quaternion[3]) == doctest::Approx(1.0f).epsilon(1.0e-4));

    // A hand that is not grabbing is ignored, and a pose with no tracker is
    // not a hand.
    CHECK_FALSE(splat::grab_hand_pose(frame, mapped.position, mapped.quaternion, false).grabbing);
    const float nan_position[3] = {std::nanf(""), 0.0f, 0.0f};
    HandPose broken = splat::grab_hand_pose(frame, nan_position, identity, true);
    CHECK_FALSE(broken.grabbing);
}

TEST_CASE("grab hand pose follows the head's yaw between the two frames") {
    // The head turned 90 degrees right in the tracker, and placed with the same
    // yaw in the placement frame: the two frames coincide again, and a hand
    // held half a metre ahead stays half a metre ahead.
    GrabFrame frame;
    const float yaw = 0.5f * kPi;
    frame.head_position[0] = 0.2f;
    frame.head_position[1] = 1.5f;
    frame.head_position[2] = -0.3f;
    // A right turn about the tracker's up axis is a negative rotation about +y.
    quat_axis(frame.head_quaternion, 1, -yaw);
    frame.head_scene.yaw = yaw;

    // The head's forward in tracker space is its local -z: 0.5 m ahead.
    const float tracker_forward[3] = {0.0f, 0.0f, -1.0f};
    float forward[3];
    rotate(frame.head_quaternion, tracker_forward, forward);
    float ahead[3];
    for (int axis = 0; axis < 3; ++axis) {
        ahead[axis] = frame.head_position[axis] + 0.5f * forward[axis];
    }

    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const HandPose mapped = splat::grab_hand_pose(frame, ahead, identity, true);
    REQUIRE(mapped.grabbing);
    // The head is the camera here (no scene offset): its placement position is
    // the camera's, and the hand is half a metre along the placement forward.
    const float expected[3] = {0.0f + 0.5f * std::sin(yaw), 0.0f,
                               0.0f + 0.5f * std::cos(yaw)};
    check_near(mapped.position, expected, 3, 1.0e-4f);

    // The same hand orientation as the head's maps to the head's own placement
    // orientation (one rotation for the whole tracker frame).
    float yaw_placement[4];
    quat_axis(yaw_placement, 1, yaw);
    const HandPose same_orientation =
        splat::grab_hand_pose(frame, frame.head_position, frame.head_quaternion, true);
    CHECK(std::fabs(same_orientation.quaternion[0] * yaw_placement[0] +
                    same_orientation.quaternion[1] * yaw_placement[1] +
                    same_orientation.quaternion[2] * yaw_placement[2] +
                    same_orientation.quaternion[3] * yaw_placement[3]) ==
          doctest::Approx(1.0f).epsilon(1.0e-4));
}

TEST_CASE("grab hand pose puts the head at the placement camera") {
    // A scene offset moves the camera away from the head's placement position,
    // so a hand at the head's own tracker position must land at camera +
    // offset, which is where ViewerState's scene adjust works from.
    GrabFrame frame;
    frame.head_position[0] = 0.0f;
    frame.head_position[1] = 0.0f;
    frame.head_position[2] = 0.0f;
    frame.head_scene.position[0] = 0.0f;
    frame.head_scene.position[1] = 0.0f;
    frame.head_scene.position[2] = 0.0f;
    frame.scene_offset[0] = 0.4f;
    frame.scene_offset[1] = -0.1f;
    frame.scene_offset[2] = 0.2f;

    const HandPose mapped = splat::grab_hand_pose(
        frame, frame.head_position, frame.head_quaternion, true);
    REQUIRE(mapped.grabbing);
    check_near(mapped.position, frame.scene_offset, 3, 1.0e-5f);
}

TEST_CASE("grab keeps the transform inside the world scale range") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float o[3] = {0.0f, 0.0f, 0.0f};
    // A transform that arrived out of range (a bug elsewhere) is brought back
    // before anything else uses it.
    const SceneTransform out = splat::update_grab(state, HandPose{}, HandPose{},
                                                 transform_of(9.0f, q, o));
    CHECK(out.scale == doctest::Approx(splat::kMaxWorldScale));
    const SceneTransform low = splat::update_grab(state, HandPose{}, HandPose{},
                                                  transform_of(0.01f, q, o));
    CHECK(low.scale == doctest::Approx(splat::kMinWorldScale));
}

TEST_CASE("grab tap undoes the press delta and steps to the previous scene") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.1f, 0.2f, 3.0f);
    // The press starts the grab at once: the first frame already takes hold.
    const SceneTransform held =
        splat::update_grab(state, hand(0.2f, 0.2f, 1.0f, q), HandPose{}, in, 10.0);
    CHECK(state.active());
    CHECK(state.take_tap() == GrabTap::none);

    // Still inside the window, a 1 cm move and a 1 degree wrist turn: the grab
    // did move the scene, as a held grab must.
    float turn[4];
    quat_axis(turn, 1, kPi / 180.0f);
    const SceneTransform moved = splat::update_grab(
        state, hand(0.21f, 0.2f, 1.0f, turn), HandPose{}, held, 10.05);
    CHECK(moved.offset[0] != doctest::Approx(in.offset[0]));

    // Release 100 ms after the press: a tap, so the press's own delta is undone
    // and the left grip asks for the previous scene.
    const SceneTransform released = splat::update_grab(state, HandPose{}, HandPose{}, moved, 10.1);
    check_near(released.offset, in.offset, 3, 1.0e-4f);
    check_near(released.rotation, in.rotation, 4, 1.0e-4f);
    CHECK(released.scale == doctest::Approx(in.scale));
    CHECK(state.take_tap() == GrabTap::previous);
    // One release is one step: the next question gets nothing.
    CHECK(state.take_tap() == GrabTap::none);
    CHECK_FALSE(state.active());
}

TEST_CASE("grab right grip tap steps forward and two hands never tap") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);
    splat::update_grab(state, HandPose{}, hand(-0.2f, 0.0f, 1.0f, q), in, 12.0);
    splat::update_grab(state, HandPose{}, hand(-0.19f, 0.0f, 1.0f, q), in, 12.05);
    splat::update_grab(state, HandPose{}, HandPose{}, in, 12.1);
    CHECK(state.take_tap() == GrabTap::next);

    // Both grips pressed and released quickly: a two-hand gesture is not a tap,
    // so no scene step is taken even with the hands still.
    GrabState both;
    const SceneTransform held =
        splat::update_grab(both, hand(-0.2f, 0.0f, 1.0f, q), hand(0.2f, 0.0f, 1.0f, q), in, 13.0);
    splat::update_grab(both, HandPose{}, HandPose{}, held, 13.1);
    CHECK(both.take_tap() == GrabTap::none);
}

TEST_CASE("grab hold outlasts the tap window and keeps its result") {
    GrabState state;
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);
    splat::update_grab(state, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 20.0);
    const SceneTransform moved =
        splat::update_grab(state, hand(0.21f, 0.0f, 1.0f, q), HandPose{}, in, 20.3);
    // Released after 300 ms: a hold, so the small move is kept as a grab.
    const SceneTransform released = splat::update_grab(state, HandPose{}, HandPose{}, moved, 20.4);
    check_near(released.offset, moved.offset, 3);
    check_near(released.rotation, moved.rotation, 4);
    CHECK(state.take_tap() == GrabTap::none);
}

TEST_CASE("grab tap needs a still hand") {
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);

    // 2.5 cm in 100 ms: more than the 2 cm tap allowance, so it is a grab.
    GrabState far;
    splat::update_grab(far, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 30.0);
    const SceneTransform pushed =
        splat::update_grab(far, hand(0.225f, 0.0f, 1.0f, q), HandPose{}, in, 30.05);
    const SceneTransform let_go = splat::update_grab(far, HandPose{}, HandPose{}, pushed, 30.1);
    check_near(let_go.offset, pushed.offset, 3);
    CHECK(far.take_tap() == GrabTap::none);

    // 4 degrees of wrist turn at the same place: also a grab.
    GrabState turned_state;
    float turned[4];
    quat_axis(turned, 1, 4.0f * kPi / 180.0f);
    splat::update_grab(turned_state, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 31.0);
    const SceneTransform twisted = splat::update_grab(
        turned_state, hand(0.2f, 0.0f, 1.0f, turned), HandPose{}, in, 31.05);
    const SceneTransform stopped =
        splat::update_grab(turned_state, HandPose{}, HandPose{}, twisted, 31.1);
    check_near(stopped.offset, twisted.offset, 3);
    check_near(stopped.rotation, twisted.rotation, 4);
    CHECK(turned_state.take_tap() == GrabTap::none);
}

TEST_CASE("grab tap window is 250 ms and a joining hand cancels it") {
    const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const SceneTransform in = transform_at(1.0f, q, 0.0f, 0.0f, 2.0f);

    // Exactly 250 ms is inside the window ("within 250 ms").
    GrabState edge;
    splat::update_grab(edge, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 40.0);
    splat::update_grab(edge, HandPose{}, HandPose{}, in, 40.25);
    CHECK(edge.take_tap() == GrabTap::previous);

    // One millisecond over it is a hold.
    GrabState over;
    splat::update_grab(over, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 41.0);
    splat::update_grab(over, HandPose{}, HandPose{}, in, 41.251);
    CHECK(over.take_tap() == GrabTap::none);

    // A second hand joining inside the window makes it a two-hand gesture: no
    // scene step, even if it is released quickly.
    GrabState joined;
    const SceneTransform held =
        splat::update_grab(joined, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in, 42.0);
    const SceneTransform both = splat::update_grab(
        joined, hand(0.2f, 0.0f, 1.0f, q), hand(0.4f, 0.0f, 1.0f, q), held, 42.05);
    splat::update_grab(joined, HandPose{}, HandPose{}, both, 42.1);
    CHECK(joined.take_tap() == GrabTap::none);

    // With no clock (the default), a release is a plain grab: the desktop's
    // existing pattern and a safe fallback if a caller has no time source.
    GrabState timeless;
    splat::update_grab(timeless, hand(0.2f, 0.0f, 1.0f, q), HandPose{}, in);
    splat::update_grab(timeless, HandPose{}, HandPose{}, in);
    CHECK(timeless.take_tap() == GrabTap::none);
}

TEST_CASE("grab hand pose shares the head's tracker-to-scene mapping") {
    // grab.cpp carries the hand through hmd_scene_position() (hmd_view.h) and
    // no longer keeps a second copy: with the head at the tracker's origin,
    // level, and its frame the placement frame, a hand's scene position is
    // exactly that mapping.
    GrabFrame frame;
    const float tracker[3] = {0.37f, 0.12f, -0.83f};
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const HandPose mapped = splat::grab_hand_pose(frame, tracker, identity, true);
    REQUIRE(mapped.grabbing);
    float expected[3];
    splat::hmd_scene_position(tracker, expected);
    check_near(mapped.position, expected, 3, 1.0e-6f);
}

TEST_CASE("grab scaling ends the viewer's scale gesture on release") {
    // The VR loop applies the grab's scale through HmdView::scale_scene(), which
    // begins a scale gesture on its own; the grab end must call
    // end_scale_gesture() so a later L2/R2 press owns its pivot and its own
    // "scale end" CSV event. This is the contract the wiring relies on (the
    // grab's release path itself is PS5-only and has no desktop entry point).
    splat::HmdView view;
    CHECK_FALSE(view.scale_gesture());
    view.scale_scene(1.5f);
    CHECK(view.scale_gesture());
    CHECK(view.world_scale() == doctest::Approx(1.5f));

    view.end_scale_gesture();
    CHECK_FALSE(view.scale_gesture());

    // The next press starts and ends cleanly, like the start/end event pair.
    view.scale_scene(2.0f);
    CHECK(view.scale_gesture());
    CHECK(view.world_scale() == doctest::Approx(3.0f));
    view.end_scale_gesture();
    CHECK_FALSE(view.scale_gesture());
}
