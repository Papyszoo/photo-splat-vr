#pragma once

// The platform-neutral part of the PS5 VR path: a tracked headset pose in
// Hmd2's convention mapped to the renderer's scene convention, the headset FOV
// turned into per-eye intrinsics, and the seated/anchor state from
// ViewerState. `platform/ps5/viewer/vr_mode.cpp` feeds it hmd2::Pose values and
// the desktop test (tests/vr_view_test.cpp) feeds it synthetic poses, so the
// view the headset gets is the code the test exercises.
//
// Hmd2/VrTracker2 pose convention (the ground truth is the HLA-PSVR2 port's
// psvr2_head(), ps5xr/runtime/src/psvr2.c:692-718): position in metres,
// quaternion (x, y, z, w) for the head frame's rotation in tracker space, +y up
// and forward along -z. The scene is OpenCV (camera.h): x right, y down and
// forward along +z, so a head frame direction d becomes (d.x, -d.y, -d.z) in
// scene axes - the same rotation (180 degrees about scene x) as HLA-PSVR2's
// per-eye view poses in OpenXR's y-up/-z-forward space.
//
// All six degrees of freedom are carried. HLA-PSVR2 hands OpenXR each *tracked*
// eye pose (position and full orientation) and the app builds its view matrix
// from it; the head's roll is in that orientation and is not optional. Yaw and
// pitch alone leave the image level with the head instead of the world: roll
// the head left and the photo tilts with it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "render/camera.h"
#include "render/overlay_quad.h"
#include "render/viewer_state.h"

namespace splat {

struct TrackedPose {
    float position[3] = {0.0f, 0.0f, 0.0f};   // tracker space, metres
    float quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
    bool tracked = false;
};

// The three poses of one tracker result, as hmd2::Poses.
struct HeadPoses {
    TrackedPose head;
    TrackedPose left;
    TrackedPose right;
};

// The first placement's stability gate: how long a worn headset's pose has to
// hold before the scene is anchored to it, and how much a single sample may
// move before that window starts again.
//
// The 2026-10-27 sessions (runs/vr7/session-20261027-003602.csv and
// -004333.csv) placed the scene on the very first frame that reported
// worn=1/tracked=1, while the tracker was still establishing its local frame.
// The SLAM reset that followed (klog: `Detect SLAM Reset`, `Map Latch`,
// `ResetLocalCoordinate at MapLatch and 6DoF`) moved the same head by 94-98 deg
// in *one* frame (0.167 s) - with no head motion to explain it - and the scene,
// anchored in the old frame, stayed there: the first recentre of those runs
// removed 90.9 and 88.6 deg of yaw, which is the "the scene is to the right
// when I start the app" report. A real head turn changes the orientation at
// most 4.1 deg per frame in the same logs; the tracker's re-anchors changed it
// 13-97 deg and the position 22-141 mm per sample. 10 deg / 40 mm separates
// them, so the scene waits for a pose that is not being redefined under it.
inline constexpr double kPlaceStableSeconds = 0.5;
inline constexpr float kPlaceJumpDegrees = 10.0f;
inline constexpr float kPlaceJumpMetres = 0.04f;

// A worn headset whose pose has held for kPlaceStableSeconds: the first
// placement's trigger. Pure and time-driven, like the recentre hold, so the
// console and the desktop test run the same code. `place` is true on exactly
// one call; a lost pose, the headset taken off, or a single jump (a tracker
// re-anchor) restarts the window.
class PlacementWait {
public:
    struct Step {
        bool place = false;             // this call completes the wait
        bool restarted = false;         // this sample broke the window
        double stable_seconds = 0.0;    // how long the current window is
    };

    Step update(bool worn, bool tracked, const TrackedPose& pose, double now_seconds);
    bool placed() const { return placed_; }
    bool waiting() const { return started_ && !placed_; }

private:
    bool started_ = false;
    bool placed_ = false;
    double since_ = 0.0;
    TrackedPose previous_;
};

// True when `b` is not the same pose as `a` by more than a head can move in one
// sample: a tracker re-anchor (a new local frame), not a fast turn.
bool pose_jumped(const TrackedPose& a, const TrackedPose& b);

// The headset's half-angle tangents, as hmd2::Fov reports them: `out` to the
// temporal side, `in` to the nasal side, `top`/`bottom` up and down. HLA-PSVR2
// builds OpenXR's per-eye FOV from the same four numbers (xr_session.c:532-548):
// the left eye is left=-out, right=+in, and the right eye has them swapped.
struct HeadsetFov {
    float out = 0.0f;
    float in = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

// One pinhole per eye plus that eye's principal-point x offset, in pixels of
// one eye image (left, right). The renderer projects both eyes from the
// midpoint, so the shared intrinsics take the midpoint centre and each eye
// gets its own offset (RendererOptions::eye_center_shift).
struct HeadsetProjection {
    CameraIntrinsics intrinsics;
    float eye_shift[2] = {0.0f, 0.0f};
};

// Pinhole intrinsics for a headset's asymmetric frustum. A degenerate FOV
// (Hmd2 returned zeros) falls back to a 60 degree horizontal field of view
// with a centred principal point, the renderer's own default.
HeadsetProjection hmd_projection(const HeadsetFov& fov, uint32_t eye_width, uint32_t eye_height);

// Head frame axes and position in scene axes. Exposed for the unit test and for
// the title's startup log; the class below is what a frame loop should use.
// `right` is the head's x axis (right ear), `down` its -y and `forward` its -z,
// all as scene vectors: an identity pose is right (1, 0, 0), down (0, 1, 0) and
// forward (0, 0, 1), the capture camera's axes.
void hmd_basis(const float quaternion[4], float right[3], float down[3], float forward[3]);
void hmd_forward(const float quaternion[4], float out[3]);
double hmd_yaw(const TrackedPose& pose);
double hmd_pitch(const TrackedPose& pose);
// Roll about the optical axis in the CameraPose convention: positive tilts the
// head's right side (right ear) down. Zero when the head's up is the world up
// for that yaw and pitch, which is what the level camera assumes.
double hmd_roll(const TrackedPose& pose);
void hmd_scene_position(const float tracker_position[3], float out[3]);

// The median forward depth (scene +z) of a scene's splat positions, sampled
// with a stride on large scenes. Scene metres. This locates the scene's centre
// on the capture camera axis: the point the viewer is looking at when the
// scene is placed at the capture pose, and the depth the log names. Falls back
// to 1.6 m for an empty or degenerate scene.
float median_scene_depth(const float* positions, size_t count);

// How close the scene's own content comes to the capture point, in scene
// metres: the 1st percentile of the positive forward depths (a percentile, not
// the minimum, so a handful of stray Gaussians cannot drag it onto the eye).
// It only bounds a scale press's pivot in the displayed space - see
// kScalePivotMetres - so "the photo starts here" is enough. Falls back to 1.6 m.
float near_scene_depth(const float* positions, size_t count);

// How far ahead of the eyes a scale press puts its pivot, metres: the point the
// scene grows or shrinks around. 1.5 m is the photo's subject distance in these
// scenes and, unlike the capture point at the eyes, it is inside the picture,
// so a press visibly changes what the user sees while they stay put.
// (Kept as the fallback when a scene has no sampled splat cloud; a measured
// scene uses the cone percentiles below.)
inline constexpr float kScalePivotMetres = 1.5f;

// VR8 scale pivots, from docs/research/vr-scale.md:
//  - front: the 5th-percentile depth of the splats in a ~15 deg cone around
//    the head's forward direction. The near foreground stays put while
//    everything behind expands - the safest read of "the scene got bigger".
//  - centre: the median depth in that cone. The middle stays put, the near
//    content moves; the factor is limited so the nearest content never comes
//    closer than kScaleNearGuardMetres. A scene that already starts inside the
//    guard (a tabletop, median <= 0.5 m) may only be shrunk by a press.
inline constexpr float kScaleConeDegrees = 15.0f;
inline constexpr float kScaleConeWideDegrees = 30.0f;
inline constexpr float kScaleFrontPercentile = 5.0f;
inline constexpr float kScaleCentrePercentile = 50.0f;
inline constexpr float kScalePivotMinMetres = 0.3f;
inline constexpr float kScalePivotMaxMetres = 30.0f;
inline constexpr float kScaleCentreMaxMetres = 60.0f;
inline constexpr float kScaleNearGuardMetres = 0.5f;

// The bounds for the centre pivot's call to std::clamp. std::clamp requires
// lo <= hi, and on a distant scene the cone's 5th-percentile depth can sit
// beyond the ceiling (p5 > kScaleCentreMaxMetres); the lower bound is then
// capped at the ceiling instead of exceeding it. Header-inline so
// tests/vr8_test.cpp can pin the ordering.
inline void centre_pivot_bounds(float p5, float* lower, float* upper) {
    *lower = std::min(std::max(p5, kScalePivotMinMetres), kScaleCentreMaxMetres);
    *upper = kScaleCentreMaxMetres;
}

// The dolly term of a press, (1-f)*D, is capped so the scene never appears to
// lurch: 1.5 m/s, the research's comfort limit.
inline constexpr float kScaleDollyMetresPerSecond = 1.5f;

// ---- maquette scaling (VR9) -----------------------------------------------
// "Like it was on my table" (headset test 6). Shrinking glides the scene's
// content centre onto a table point T frozen at the press's start, and L2/R2
// then grow and shrink the maquette about T. The numbers are the brief's:
// T ~0.7 m ahead along the horizontal gaze and ~0.35 m below eye height, and
// the robust (5-95 percentile) box is ~0.6 m wide at the maquette scale s_m.
inline constexpr float kMaquetteTableForwardMetres = 0.7f;
inline constexpr float kMaquetteTableBelowEyesMetres = 0.35f;
inline constexpr float kMaquetteWidthMetres = 0.6f;
// How close to 1.0 the scale counts as "back at full size": a round trip that
// ends there finishes the maquette, so the next press captures a fresh table
// point and content centre. Float accumulation makes an exact 1.0 rare.
inline constexpr float kMaquetteCaptureEpsilon = 1.0e-4f;

// The glide's blend for an absolute world scale, t = smoothstep((1-s)/(1-s_m)):
// 0 at s = 1 (the original placement) and 1 at and below s_m (on the table).
// A scene whose robust extent is already under kMaquetteWidthMetres has
// s_m = 1 and no glide (t is 0 at 1, 1 below it).
float maquette_blend(float scale, float scale_min);

// The content centre C(s) in the placement frame. Above s = 1 the scene scales
// about T (C = T + s(C0 - T), continuous at C(1) = C0); between s_m and 1 it
// glides from C0 to T with maquette_blend(); below s_m it stays on T, which is
// exactly scaling about T. `c0` is C(1), the robust centre measured at the
// press's start; `table` is T.
void maquette_centre(float scale, const float c0[3], const float table[3], float scale_min,
                     float out[3]);

// s_m: the scale at which a robust horizontal extent E becomes
// kMaquetteWidthMetres wide, clamped to the world-scale range and never above
// 1 (a scene smaller than the table never needs shrinking to fit).
float maquette_scale_for_extent(float extent_metres);

// The robust horizontal extent of a scene-space splat cloud at `scale`: the
// larger of the x and z 5-95 percentile spans, in displayed metres. Falls back
// to 0 (no samples), which maquette_scale_for_extent() reads as "no glide".
float robust_scene_extent(const float* positions, size_t count, float scale);

enum class ScalePivotMode { front = 0, centre = 1, maquette = 2 };

// The lazy-follow VR panel (headset test 5): a world-stable point at a fixed
// distance ahead of the eyes, re-centred with a ~0.3 s ease when the head has
// turned more than ~17 deg away from it, and always drawn parallel to the
// view (yaw and pitch, never roll).
//
// Why not a classic eye-point billboard: a quad whose normal points at the eye
// from an off-axis centre is seen at an angle, and that angle *is* the
// keystone. Keeping the quad's plane parallel to the view plane makes the
// projected shape a rigid rectangle for any head yaw, pitch or roll; the
// direction vector only decides where it sits. Re-centring moves that
// direction back in front of the user without changing the shape.
inline constexpr float kPanelDistanceMetres = 1.6f;
inline constexpr float kPanelFollowStartDegrees = 17.5f;
inline constexpr float kPanelFollowSettleDegrees = 1.5f;
// First-order ease on the direction: 95 % of the way in 0.3 s.
inline constexpr double kPanelFollowSeconds = 0.1;

class PanelFollower {
public:
    // The panel appears for the first time: directly ahead of this pose.
    void reset(double yaw, double pitch);
    // One frame: ease the direction back in front when it has left the view.
    void update(double yaw, double pitch, double seconds);
    // The quad for this frame at `eye`: centre on the followed direction, the
    // plane parallel to the current yaw/pitch (upright, no roll).
    OverlayQuad quad(const float eye[3], double yaw, double pitch) const;

    const float* direction() const { return direction_; }
    bool following() const { return following_; }
    bool initialised() const { return initialised_; }
    float distance = kPanelDistanceMetres;

private:
    float direction_[3] = {0.0f, 0.0f, 1.0f};
    bool following_ = false;
    bool initialised_ = false;
};

// Tracked head -> FrameView, with the placement/recentre semantics of the
// viewer: place() puts the capture camera (the scene origin) between the eyes
// and makes the head's frame the scene's frame (its yaw becomes scene yaw zero,
// its pitch scene level and its roll scene upright), so the first frame is the
// capture pose looking forward. update() follows the head afterwards; a lost
// tracker holds the last pose. place() is the recentre path - the first
// placement and the Cross hold both end here (PlacementWait decides *when* the
// first one happens, so neither can place from a pose the tracker is about to
// redefine).
//
// Everything after that is one rigid transform: the scene frame is the head's
// frame at the last place(), so the camera is the head's position and
// orientation expressed in it (R_placement^T * pose). That matters for all six
// degrees of freedom, not just rotation: with the placement pitched, a head step
// along the head's x axis must move the camera along the camera's x axis, which
// only a single shared rotation gives. HLA-PSVR2 does the same thing with its
// LOCAL space (xr_session.c:141-166); its space is yaw-only because OpenXR apps
// expect gravity, ours is the full placement rotation because the photo has no
// gravity and must appear in front of the eyes.
//
// The scene can be manipulated without moving the anchor. A scale press (L2/R2)
// freezes a pivot in the *world*. VR9's default is the **maquette**: T is a
// table point 0.7 m ahead and 0.35 m below the eyes, the scene shrinks and its
// content centre glides onto T, then L2/R2 scale the maquette about T; growing
// past the landing scale glides it back to the original placement, and above 1
// it also scales about T. T, C0 and the landing scale belong to the round trip,
// not to one press: they are captured when a shrink starts from full size and
// reused by every later press until a recentre, a scene change, or the scale
// returning to 1.0, so several presses down and then up return exactly to the
// original placement. VR8's Front (5th-percentile cone depth) and Centre
// (median, 0.5 m near guard) pivots remain as Settings options. The viewer
// does not move: the camera is the head and only the scene transforms change
// (ViewerState::scale_about_pivot(), or place_maquette() for the glide). VR7
// used a fixed 1.5 m point, which was still read as a dolly; VR6 scaled about
// the capture point at the eyes, a pure zoom ("scale does absolutely nothing").
// VR5 instead compensated a pivot 25-50 m away and threw the user 139 m
// (runs/vr5/REPORT.md, runs/vr6/REPORT.md, docs/research/vr-scale.md,
// docs/PLAN.md "Headset test 6").
// move_viewer()/turn_viewer() are locomotion: the sticks slide the world the
// other way, so the user walks (forward/right), rises (up) and turns to their
// right. All of it is cumulative and lives in ViewerState; recentre resets it
// (see reset_scene_adjust()). A scene switch changes none of it - only
// set_median_depth()/set_near_depth() of the new scene.
//
// The comfort radius is measured as physical head travel from the anchor, in
// metres (see ViewerState).
class HmdView {
public:
    // The VR comfort default (kDefaultVrComfortRadius), not the desktop's
    // 0.25 m: the scene is world-locked and the user stands in it.
    HmdView();

    // Clamped by ViewerState. The first value is the scale recentre returns to.
    void set_comfort_radius(float radius);
    void set_world_scale(float scale);
    void set_fade_start(float fraction);
    float comfort_radius() const { return state_.comfort_radius(); }
    float world_scale() const { return state_.world_scale(); }

    // Updates the cached head pose and eye midpoint; false when the head is
    // not tracked, in which case the last pose is held.
    bool update(const HeadPoses& poses);
    // update(), then re-anchor: capture camera at the eyes, and the head's
    // frame (yaw, pitch and roll) becomes the scene's frame. False when
    // untracked.
    bool place(const HeadPoses& poses);
    // Same as place(), for the recentre hold. Does not touch the scene
    // adjustments; call reset_scene_adjust() with it.
    bool recentre(const HeadPoses& poses) { return place(poses); }
    // Re-anchors at the cached pose, without a new tracker sample (a scene
    // switch keeps the head pose it already has).
    void recentre_here();
    // Back to the world the capture camera saw: the configured scale, no scene
    // offset and no scene turn (recentre's completion).
    void reset_scene_adjust();

    // ---- scene manipulation (sticks and triggers) -------------------------
    // The scene's median forward depth in scene metres. Set once per loaded
    // scene; it moves nothing, so a scene switch keeps the world placement.
    void set_median_depth(float depth);
    float median_depth() const { return median_depth_; }
    // The scene's centre as a point of the scene itself: (0, 0, median depth),
    // i.e. on the capture camera axis at the depth the user is looking at when
    // placed at the capture pose. Not where the user looks, not the eye.
    const float* scale_pivot() const { return scale_pivot_; }
    // The nearest content of the loaded scene, in scene metres, for the scale
    // pivot's clamp (0 or less keeps the default).
    void set_near_depth(float depth);
    float near_depth() const { return near_depth_; }

    // ---- scale (L2/R2), about a point fixed in the world ------------------
    // Which pivot the next press freezes: Front (the default) or Centre.
    void set_scale_pivot_mode(ScalePivotMode mode) { pivot_mode_ = mode; }
    ScalePivotMode scale_pivot_mode() const { return pivot_mode_; }
    // A decimated copy of the scene's splat positions (scene space, xyz),
    // measured once per loaded scene. Presses pick their cone from these; with
    // none set the VR7 fixed pivot is the fallback.
    void set_pivot_samples(std::vector<float> positions) {
        pivot_samples_ = std::move(positions);
    }
    void clear_pivot_samples() { pivot_samples_.clear(); }
    size_t pivot_sample_count() const { return pivot_samples_.size() / 3; }

    // Freezes this press's pivot. Front: the 5th-percentile depth in a 15 deg
    // cone around the head forward (widened to 30 deg, then the whole scene,
    // when too few splats are there). Centre: the median depth in that cone,
    // with the near content guard applied in scale_scene(). Fallback with no
    // samples: 1.5 m ahead on the horizontal gaze, clamped to the near content.
    // Maquette: the first press of a round trip also freezes T, C0 and s_m; a
    // later press reuses them until a recentre, a scene change, or the scale
    // having returned to 1.0 (the round trip's end).
    void begin_scale_gesture();
    // Ends the press; the next one picks its own pivot.
    void end_scale_gesture() { scale_gesture_ = false; }
    bool scale_gesture() const { return scale_gesture_; }
    // The pivots of the current (or last) press: as a world point in the
    // placement frame, and its distance from the eyes along the gaze. In
    // maquette mode this is the table point T (the marker and the readout use
    // it), and scale_pivot_distance() is the distance from the eyes to T.
    const float* scale_pivot_world() const { return pivot_world_; }
    float scale_pivot_distance() const { return pivot_distance_; }
    // The cone's nearest-content depth at press start (displayed metres).
    float scale_cone_near() const { return cone_near_; }
    // Maquette mode (VR9), frozen at the round trip's start (not per press): the
    // table point T, the content centre C0 the scene's splats have at s = 1, the
    // scene-space point that sits at C0 (so the offset can be written directly),
    // s_m and the robust extent it came from. Meaningless before the first
    // maquette press; maquette_gesture() says whether they describe the current
    // press.
    const float* maquette_table() const { return maquette_table_; }
    const float* maquette_content_centre() const { return maquette_centre_; }
    const float* maquette_anchor() const { return maquette_anchor_; }
    float maquette_scale() const { return maquette_scale_; }
    float maquette_extent() const { return maquette_extent_; }
    bool maquette_gesture() const {
        return scale_gesture_ && pivot_mode_ == ScalePivotMode::maquette;
    }
    // Uniform scale by `factor` for this press. Front/Centre scale about the
    // frozen cone pivot; Maquette writes the absolute placement for the new
    // scale instead (the glide onto T, then scaling about T). The viewer does
    // not move in either case, and the scene's offset absorbs the difference so
    // lift/move/turn compose with it exactly. `seconds` (when > 0) is the frame
    // dt: the dolly term (1-f)*D - or, in Maquette, the content centre's travel
    // - is capped at kScaleDollyMetresPerSecond, and Centre mode stops growing
    // before the nearest content reaches kScaleNearGuardMetres. Clamped by
    // ViewerState's range.
    void scale_scene(float factor, double seconds = 0.0);

    // ---- locomotion (sticks) ----------------------------------------------
    // Moves the viewer by (right, forward, up) in the head's horizontal frame
    // (yaw only, no pitch) and turns the viewer to their right by `radians`.
    // The world moves the other way, so this is the inverse of the scene
    // adjustments below: the forward stick walks into the scene.
    void move_viewer(float right, float forward, float up);
    void turn_viewer(float radians);
    // The scene-side primitives the locomotion is built from, and what the
    // scene label/desktop viewer use: `right` slides the scene to the head's
    // right, `forward` away from the head, `up` up along the placement frame;
    // a positive turn turns the *scene* to the user's right.
    void move_scene(float right, float forward, float up);
    void turn_scene(float radians);
    float scene_turn() const { return turn_; }
    const float* scene_offset() const { return state_.scene_offset(); }

    // The frame for the cached pose.
    FrameView frame() const;

    float ipd() const { return ipd_; }
    // The head's own angles in scene axes (what the tracker reports).
    double head_yaw() const { return head_yaw_; }
    double head_pitch() const { return head_pitch_; }
    double head_roll() const { return head_roll_; }
    // The angles the renderer is given: the head's, with the placement rotation
    // removed. These are the yaw/pitch/roll of a FrameView.
    double scene_yaw() const { return scene_yaw_; }
    double scene_pitch() const { return scene_pitch_; }
    double scene_roll() const { return scene_roll_; }
    // The yaw the renderer is actually given: the scene yaw with the right-stick
    // turn removed (a positive turn swings the view left, so the content turns
    // right). This is the value the pose log's `yaw_deg` column records; the
    // turn itself is the separate `turn_deg` column (scene_turn()).
    double view_yaw() const { return scene_yaw_ - static_cast<double>(turn_); }
    const float* head_position() const { return head_; }
    const float* anchor() const { return state_.anchor(); }

private:
    // scene_yaw_/pitch_/roll_ from the cached head axes in the placement frame.
    void update_scene_angles();
    // The eye midpoint in the placement frame, with the scene offset applied.
    void camera_position(float out[3]) const;
    // The eye midpoint in the placement frame, with no scene offset: the user's
    // head in the space ViewerState's scale/offset work in.
    void placement_head(float out[3]) const;
    // The direction the renderer's camera looks, in the placement frame: the
    // head's scene yaw/pitch with the right-stick turn applied.
    void view_forward(float out[3]) const;
    // This press's distance along view_forward(): the cone percentile for the
    // selected mode. Returns 0 when there is no sample cloud (the caller keeps
    // the VR7 fallback) and fills `cone_near` with the cone's near content.
    float cone_pivot_distance(float* cone_near) const;
    // The robust centre of the visible cone (per-axis median of the displayed
    // splat positions, widened 15 -> 30 deg -> whole scene as the pivot
    // measure does). False when the scene has no sample cloud.
    bool cone_content_centre(float out[3]) const;
    // Writes the scale/offset for an absolute maquette scale: the world scale
    // and the offset that puts the frozen content anchor at C(s). Returns the
    // applied scale.
    float place_maquette(float scale);

    ViewerState state_;
    float head_[3] = {0.0f, 0.0f, 0.0f};       // eye midpoint, scene axes
    float head_right_[3] = {1.0f, 0.0f, 0.0f};   // the head's axes, scene axes
    float head_down_[3] = {0.0f, 1.0f, 0.0f};
    float head_forward_[3] = {0.0f, 0.0f, 1.0f};
    // The same three axes at the last place(): the rotation from the scene frame
    // into the head's frame, i.e. the columns of R_placement (identity until the
    // first place(), which leaves the head pose as it is).
    float placement_right_[3] = {1.0f, 0.0f, 0.0f};
    float placement_down_[3] = {0.0f, 1.0f, 0.0f};
    float placement_forward_[3] = {0.0f, 0.0f, 1.0f};
    double head_yaw_ = 0.0;
    double head_pitch_ = 0.0;
    double head_roll_ = 0.0;
    double scene_yaw_ = 0.0;
    double scene_pitch_ = 0.0;
    double scene_roll_ = 0.0;
    float ipd_ = 0.063f;
    // Scene manipulation: the turn the right stick adds (about the placement
    // frame's vertical; the camera yaw is scene_yaw - turn_), the median depth
    // of the loaded scene (with the scene point it locates), the near content
    // of the loaded scene, this press's world pivot, and the scale recentre
    // returns to.
    float turn_ = 0.0f;
    float median_depth_ = 1.6f;
    float scale_pivot_[3] = {0.0f, 0.0f, 1.6f};
    float near_depth_ = 1.6f;
    float pivot_world_[3] = {0.0f, 0.0f, 1.5f};
    float pivot_distance_ = kScalePivotMetres;
    float cone_near_ = 0.0f;
    // Maquette mode's frozen table point, content centre and its scene-space
    // anchor, s_m and the robust extent at the round trip's start (VR9), and
    // whether that capture is still current.
    float maquette_table_[3] = {0.0f, 0.35f, 0.7f};
    float maquette_centre_[3] = {0.0f, 0.0f, 1.6f};
    float maquette_anchor_[3] = {0.0f, 0.0f, 1.6f};
    float maquette_scale_ = 1.0f;
    float maquette_extent_ = 0.0f;
    bool maquette_captured_ = false;
    float gesture_start_scale_ = 1.0f;
    // Centre mode's factor ceiling for the near-content guard; 0 = no limit.
    float centre_growth_limit_ = 0.0f;
    ScalePivotMode pivot_mode_ = ScalePivotMode::front;
    // A decimated copy of the scene's splat positions (scene space xyz): the
    // cone measure is taken per press from these, not per frame.
    std::vector<float> pivot_samples_;
    bool scale_gesture_ = false;
    float initial_scale_ = 1.0f;
};

}  // namespace splat
