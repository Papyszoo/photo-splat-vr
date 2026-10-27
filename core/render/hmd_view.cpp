// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/hmd_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace splat {

void hmd_basis(const float quaternion[4], float right[3], float down[3], float forward[3]) {
    const float x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];
    // The rotation's columns are the head frame's x (right ear), y (up) and z
    // (backward) axes in tracker space; the scene mapping (x, -y, -z) is a
    // rotation by 180 degrees about x, applied to each column. down is -up.
    const float tracker_right[3] = {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z),
                                    2.0f * (x * z - w * y)};
    const float tracker_up[3] = {2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z),
                                 2.0f * (y * z + w * x)};
    if (right != nullptr) {
        right[0] = tracker_right[0];
        right[1] = -tracker_right[1];
        right[2] = -tracker_right[2];
    }
    if (down != nullptr) {
        down[0] = -tracker_up[0];
        down[1] = tracker_up[1];
        down[2] = tracker_up[2];
    }
    if (forward != nullptr) hmd_forward(quaternion, forward);
}

void hmd_forward(const float quaternion[4], float out[3]) {
    const float x = quaternion[0], y = quaternion[1], z = quaternion[2], w = quaternion[3];
    // Head forward in tracker space is R * (0, 0, -1), then the scene mapping
    // (x, -y, -z) turns it into the renderer's OpenCV axes. An identity pose is
    // (0, 0, 1): level and down +z, the capture camera's view.
    out[0] = -2.0f * (x * z + y * w);
    out[1] = 2.0f * (y * z - w * x);
    out[2] = 1.0f - 2.0f * (x * x + y * y);
}

double hmd_yaw(const TrackedPose& pose) {
    float forward[3];
    hmd_forward(pose.quaternion, forward);
    // CameraPose yaw is positive to the right: f = (sin yaw, 0, cos yaw).
    return static_cast<double>(std::atan2(forward[0], forward[2]));
}

double hmd_pitch(const TrackedPose& pose) {
    float forward[3];
    hmd_forward(pose.quaternion, forward);
    // The scene's y axis points down, so a forward with +y looks down.
    return static_cast<double>(std::asin(std::max(-1.0f, std::min(1.0f, forward[1]))));
}

namespace {

// The camera angles of one orthonormal camera basis in scene axes, as the
// renderer builds its view matrix (splat_renderer.cpp): the forward vector gives
// yaw and pitch, then the level pair s0 = (cos yaw, 0, -sin yaw) and
// d0 = f x s0 gives the roll from where the camera's right axis sits between
// them (cos(roll) s0 + sin(roll) d0). Exact for any rotation: this is the
// yaw -> pitch -> roll decomposition of CameraPose, not an approximation.
void decompose_axes(const float right[3], const float forward[3], double* yaw,
                    double* pitch, double* roll) {
    *yaw = std::atan2(forward[0], forward[2]);
    *pitch = std::asin(std::max(-1.0f, std::min(1.0f, forward[1])));
    const float sy = std::sin(static_cast<float>(*yaw));
    const float cy = std::cos(static_cast<float>(*yaw));
    const float s0[3] = {cy, 0.0f, -sy};
    const float d0[3] = {forward[1] * s0[2] - forward[2] * s0[1],
                         forward[2] * s0[0] - forward[0] * s0[2],
                         forward[0] * s0[1] - forward[1] * s0[0]};
    const float along = right[0] * s0[0] + right[1] * s0[1] + right[2] * s0[2];
    const float tilt = right[0] * d0[0] + right[1] * d0[1] + right[2] * d0[2];
    *roll = std::atan2(tilt, along);
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The splats of VR8/VR9's visible cone in the displayed space w = s*x + o: a
// 15 deg cone around the head forward, widened to 30 deg and then to the whole
// scene (a photo seen edge-on, or a tiny scene) when a pass keeps fewer than
// 200 samples. The per-axis displayed positions and the forward depths are
// filled only when their pointer is non-null, so the pivot measure takes the
// depths alone and the content centre takes the positions alone - one loop for
// the two callers that used to carry a copy each (review fix). Returns the
// number of samples kept; 0 for an empty cloud.
size_t collect_cone_samples(const std::vector<float>& positions, const float head[3],
                            const float forward[3], float scale, const float* offset,
                            std::vector<float>* xs, std::vector<float>* ys,
                            std::vector<float>* zs, std::vector<float>* depths) {
    constexpr size_t kEnough = 200;
    const size_t count = positions.size() / 3;
    auto reset = [](std::vector<float>* out) {
        if (out != nullptr) out->clear();
    };
    if (count == 0) {
        reset(xs);
        reset(ys);
        reset(zs);
        reset(depths);
        return 0;
    }
    const float cos_start[2] = {
        std::cos(kScaleConeDegrees * 3.14159265358979323846f / 180.0f),
        std::cos(kScaleConeWideDegrees * 3.14159265358979323846f / 180.0f)};
    size_t kept = 0;
    for (int pass = 0; pass < 3; ++pass) {
        reset(xs);
        reset(ys);
        reset(zs);
        reset(depths);
        kept = 0;
        const bool all = pass == 2;
        for (size_t i = 0; i < count; ++i) {
            const float x = positions[i * 3 + 0] * scale + offset[0];
            const float y = positions[i * 3 + 1] * scale + offset[1];
            const float z = positions[i * 3 + 2] * scale + offset[2];
            const float dx = x - head[0];
            const float dy = y - head[1];
            const float dz = z - head[2];
            const float length2 = dx * dx + dy * dy + dz * dz;
            if (!(length2 > 1.0e-6f)) continue;
            const float depth = dx * forward[0] + dy * forward[1] + dz * forward[2];
            if (!(depth > 0.05f)) continue;
            if (!all) {
                const float cosine = depth / std::sqrt(length2);
                if (cosine < cos_start[pass]) continue;
            }
            if (xs != nullptr) xs->push_back(x);
            if (ys != nullptr) ys->push_back(y);
            if (zs != nullptr) zs->push_back(z);
            if (depths != nullptr) depths->push_back(depth);
            ++kept;
        }
        if (kept >= kEnough || all) break;
    }
    return kept;
}

}  // namespace

double hmd_roll(const TrackedPose& pose) {
    float right[3];
    float forward[3];
    hmd_basis(pose.quaternion, right, nullptr, forward);
    double yaw = 0.0, pitch = 0.0, roll = 0.0;
    decompose_axes(right, forward, &yaw, &pitch, &roll);
    return roll;
}

void hmd_scene_position(const float tracker_position[3], float out[3]) {
    out[0] = tracker_position[0];
    out[1] = -tracker_position[1];
    out[2] = -tracker_position[2];
}

bool pose_jumped(const TrackedPose& a, const TrackedPose& b) {
    const float dx = b.position[0] - a.position[0];
    const float dy = b.position[1] - a.position[1];
    const float dz = b.position[2] - a.position[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > kPlaceJumpMetres) return true;
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    for (int i = 0; i < 4; ++i) {
        dot += static_cast<double>(a.quaternion[i]) * static_cast<double>(b.quaternion[i]);
        norm_a += static_cast<double>(a.quaternion[i]) * static_cast<double>(a.quaternion[i]);
        norm_b += static_cast<double>(b.quaternion[i]) * static_cast<double>(b.quaternion[i]);
    }
    const double scale = std::sqrt(norm_a * norm_b);
    if (!(scale > 0.0)) return true;   // a zero quaternion is not a pose
    dot = std::min(1.0, std::max(-1.0, dot / scale));
    // The angle between two orientations: 2 acos|dot|, in degrees.
    const double degrees = 2.0 * std::acos(std::fabs(dot)) * 180.0 / 3.14159265358979323846;
    return degrees > static_cast<double>(kPlaceJumpDegrees);
}

PlacementWait::Step PlacementWait::update(bool worn, bool tracked, const TrackedPose& pose,
                                          double now_seconds) {
    Step step;
    if (placed_) return step;
    if (!worn || !tracked) {
        // Not worn yet, or the tracker has no pose: nothing to hold on to.
        started_ = false;
        return step;
    }
    if (!started_) {
        started_ = true;
        since_ = now_seconds;
        previous_ = pose;
        return step;
    }
    if (pose_jumped(previous_, pose)) {
        // A new local frame (or the headset was moved in the hand): the pose
        // this window was building towards is not the one the user is at.
        step.restarted = true;
        since_ = now_seconds;
    }
    previous_ = pose;
    step.stable_seconds = std::max(0.0, now_seconds - since_);
    if (step.stable_seconds >= kPlaceStableSeconds) {
        placed_ = true;
        step.place = true;
    }
    return step;
}

float maquette_blend(float scale, float scale_min) {
    if (!std::isfinite(scale) || !std::isfinite(scale_min)) return 0.0f;
    if (!(scale_min < 1.0f)) return scale >= 1.0f ? 0.0f : 1.0f;
    float t = (1.0f - scale) / (1.0f - scale_min);
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

void maquette_centre(float scale, const float c0[3], const float table[3], float scale_min,
                     float out[3]) {
    if (!std::isfinite(scale)) scale = 1.0f;
    // Above 1 the scene grows about T (the same pivot the maquette itself uses);
    // C(1) = C0 keeps the glide continuous into it.
    if (scale > 1.0f) {
        for (int axis = 0; axis < 3; ++axis) {
            out[axis] = table[axis] + scale * (c0[axis] - table[axis]);
        }
        return;
    }
    const float t = maquette_blend(scale, scale_min);
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = c0[axis] + (table[axis] - c0[axis]) * t;
    }
}

float maquette_scale_for_extent(float extent_metres) {
    if (!(extent_metres > 1.0e-4f) || !std::isfinite(extent_metres)) return 1.0f;
    const float scale = kMaquetteWidthMetres / extent_metres;
    return std::max(kMinWorldScale, std::min(1.0f, scale));
}

float robust_scene_extent(const float* positions, size_t count, float scale) {
    if (positions == nullptr || count == 0) return 0.0f;
    const size_t step = std::max<size_t>(1, count / 20000);
    std::vector<float> xs;
    std::vector<float> zs;
    xs.reserve(count / step + 1);
    zs.reserve(count / step + 1);
    for (size_t i = 0; i < count; i += step) {
        const float x = positions[i * 3 + 0];
        const float z = positions[i * 3 + 2];
        if (!std::isfinite(x) || !std::isfinite(z)) continue;
        xs.push_back(x * scale);
        zs.push_back(z * scale);
    }
    if (xs.empty()) return 0.0f;
    auto span = [](std::vector<float>& values) {
        const size_t low = static_cast<size_t>(0.05f * static_cast<float>(values.size() - 1));
        const size_t high = static_cast<size_t>(0.95f * static_cast<float>(values.size() - 1));
        std::nth_element(values.begin(), values.begin() + static_cast<long>(low), values.end());
        const float p5 = values[low];
        std::nth_element(values.begin(), values.begin() + static_cast<long>(high), values.end());
        return values[high] - p5;
    };
    const float width_x = span(xs);
    const float width_z = span(zs);
    return std::max(width_x, width_z);
}

float median_scene_depth(const float* positions, size_t count) {
    constexpr float kFallback = 1.6f;
    if (positions == nullptr || count == 0) return kFallback;
    // One sample in `step` keeps a 1.18 M-splat scene at about 20k values, far
    // more than a median needs, and only every splat costs 3 floats to touch.
    const size_t step = std::max<size_t>(1, count / 20000);
    std::vector<float> depths;
    depths.reserve(count / step + 1);
    for (size_t i = 0; i < count; i += step) {
        const float z = positions[i * 3 + 2];
        // Behind or at the capture camera is not "in front": a few Gaussians
        // of a SHARP scene sit slightly behind the image plane and must not
        // drag the median to zero.
        if (std::isfinite(z) && z > 0.05f) depths.push_back(z);
    }
    if (depths.empty()) return kFallback;
    const size_t middle = depths.size() / 2;
    std::nth_element(depths.begin(), depths.begin() + middle, depths.end());
    return std::clamp(depths[middle], 0.2f, 50.0f);
}

float near_scene_depth(const float* positions, size_t count) {
    constexpr float kFallback = 1.6f;
    if (positions == nullptr || count == 0) return kFallback;
    const size_t step = std::max<size_t>(1, count / 20000);
    std::vector<float> depths;
    depths.reserve(count / step + 1);
    for (size_t i = 0; i < count; i += step) {
        const float z = positions[i * 3 + 2];
        if (std::isfinite(z) && z > 0.05f) depths.push_back(z);
    }
    if (depths.empty()) return kFallback;
    // The 1st percentile: the photo's own near edge, without letting a handful
    // of stray Gaussians (a splat on the lens) put the pivot on the user's nose.
    const size_t near_index = depths.size() / 100;
    std::nth_element(depths.begin(), depths.begin() + near_index, depths.end());
    // 0.25 m floor: closer than that the pivot would sit inside the near plane
    // and a press would scale the user's own toes.
    return std::clamp(depths[near_index], 0.25f, 50.0f);
}

HeadsetProjection hmd_projection(const HeadsetFov& fov, uint32_t eye_width, uint32_t eye_height) {
    const float width = static_cast<float>(eye_width);
    const float height = static_cast<float>(eye_height);
    HeadsetProjection projection;
    const float horizontal = fov.out + fov.in;   // r - l, the full x span
    const float vertical = fov.top + fov.bottom;
    if (!(horizontal > 0.0f) || !(vertical > 0.0f)) {
        const float fx = 0.5f * width / std::tan(30.0f * 3.14159265358979f / 180.0f);
        projection.intrinsics = {fx, fx, 0.5f * width, 0.5f * height};
        projection.eye_shift[0] = projection.eye_shift[1] = 0.0f;
        return projection;
    }
    // One rectilinear pinhole per eye, covering exactly the frustum the
    // tangents describe: x = -out at pixel 0 and +in at pixel W for the left
    // eye, the same numbers Hmd2 gets in SetParam and reprojects with
    // (hmd2.cpp build_frame_layer; HLA-PSVR2 passes its app's own FOV the same
    // way, xr_session.c:532-548, xr_vulkan.c:657-662). fx and fy come out
    // different (711.4 vs 770.5 px on PSVR2) because this eye image's angular
    // spans are not in the panel's aspect ratio; that is the headset's own
    // geometry, not a bug to equalise.
    //
    // Vertical: the top of the eye image is the -top tangent and the bottom the
    // +bottom one, so cy = H * top / (top + bottom) (H/2 when they are equal,
    // which is what PSVR2 reports). Both eyes share it.
    projection.intrinsics = {width / horizontal, height / vertical, 0.5f * width,
                             height * fov.top / vertical};
    // Left eye: l = -out, r = +in, so cx_L = W * out / (out + in) - the
    // principal point sits on the nasal side of the panel's centre, because
    // the temporal field is the wide one.
    // Right eye: l = -in, r = +out, so cx_R = W * in / (out + in) = W - cx_L.
    const float left_center = width * fov.out / horizontal;
    projection.eye_shift[0] = left_center - 0.5f * width;
    projection.eye_shift[1] = -projection.eye_shift[0];
    return projection;
}

HmdView::HmdView() { state_.set_comfort_radius(kDefaultVrComfortRadius); }

void HmdView::set_comfort_radius(float radius) { state_.set_comfort_radius(radius); }
void HmdView::set_world_scale(float scale) {
    initial_scale_ = scale;
    state_.set_world_scale(scale);
    // A new baseline placement: the next maquette press captures its own T/C0.
    maquette_captured_ = false;
}
void HmdView::set_fade_start(float fraction) { state_.set_fade_start(fraction); }

void HmdView::set_median_depth(float depth) {
    median_depth_ = std::isfinite(depth) && depth > 0.0f ? depth : 1.6f;
    // The scene's centre, as a point of the scene: the capture camera axis at
    // the median depth. A new scene brings its own; nothing else changes.
    scale_pivot_[0] = 0.0f;
    scale_pivot_[1] = 0.0f;
    scale_pivot_[2] = median_depth_;
    // A new scene: the next maquette press must not reuse the old scene's table
    // point, content centre or landing scale.
    maquette_captured_ = false;
}

void HmdView::set_near_depth(float depth) {
    near_depth_ = std::isfinite(depth) && depth > 0.0f ? depth : 1.6f;
    maquette_captured_ = false;
}

void HmdView::begin_scale_gesture() {
    // The pivot: a point of the world, so it survives a head turn inside the
    // press and grow-then-shrink returns exactly. VR8 measures it from a
    // decimated copy of the scene's splats in a cone around the direction the
    // renderer's camera looks (yaw, pitch and the right-stick turn included):
    //  - Front (default): the 5th-percentile depth keeps the foreground put;
    //  - Centre: the median depth keeps the middle put, with a near-content
    //    guard applied per frame in scale_scene().
    float head[3];
    placement_head(head);
    float direction[3];
    view_forward(direction);
    float cone_near = 0.0f;
    float distance = cone_pivot_distance(&cone_near);
    if (!(distance > 0.0f)) {
        // No sample cloud (a scene the viewer has not measured): the VR7
        // fallback, 1.5 m ahead along the horizontal gaze, clamped to the near
        // content. Keeps the old behaviour exact and the desktop tests that do
        // not build a scene point cloud deterministic.
        const double yaw = view_yaw();
        direction[0] = static_cast<float>(std::sin(yaw));
        direction[1] = 0.0f;
        direction[2] = static_cast<float>(std::cos(yaw));
        distance = kScalePivotMetres;
        const float near_world = state_.world_scale() * near_depth_;
        if (near_world > 0.05f) distance = std::min(distance, near_world);
        distance = std::max(distance, 0.05f);
        cone_near = near_world;
    }
    // Front mode can sit below/above eye level when the user looks at a table;
    // the pivot is still a world point and the formula is unchanged.
    for (int axis = 0; axis < 3; ++axis) {
        pivot_world_[axis] = head[axis] + direction[axis] * distance;
    }
    pivot_distance_ = distance;
    cone_near_ = cone_near;
    gesture_start_scale_ = state_.world_scale();
    centre_growth_limit_ = 0.0f;

    if (pivot_mode_ == ScalePivotMode::maquette) {
        // VR9: T, C0 and s_m belong to the round trip, not to one press (review
        // fix). They are captured when a shrink starts from full size - the
        // first maquette press after a recentre or a scene change, and again
        // once the scale has returned to 1.0 because that ends the round trip
        // at the original placement - and every later press reuses them. A
        // second, separate press at 0.5x therefore keeps the table point it
        // landed on, and growing back in further presses reaches s = 1 and the
        // exact original offset. Deriving them per press was the bug: s_m came
        // from the *displayed* extent (0.6 / (E * s), twice too large at 0.5x)
        // and C0 from the maquette's table position, so the glide home could
        // never return.
        if (!maquette_captured_ || state_.world_scale() >= 1.0f - kMaquetteCaptureEpsilon) {
            // T: 0.7 m ahead along the horizontal look direction and 0.35 m
            // below the eyes (scene y is down, so +0.35). The marker's point and
            // the pivot the maquette grows/shrinks about once it has landed.
            const double yaw = view_yaw();
            maquette_table_[0] = head[0] + static_cast<float>(std::sin(yaw)) *
                                              kMaquetteTableForwardMetres;
            maquette_table_[1] = head[1] + kMaquetteTableBelowEyesMetres;
            maquette_table_[2] = head[2] + static_cast<float>(std::cos(yaw)) *
                                              kMaquetteTableForwardMetres;
            // C0: the robust centre of the visible cone. Without a sample cloud,
            // the scene point on the gaze at the median depth is the best centre
            // available; a scene smaller than the table keeps s_m = 1.
            if (!cone_content_centre(maquette_centre_)) {
                maquette_centre_[0] = head[0] + direction[0] * median_depth_;
                maquette_centre_[1] = head[1] + direction[1] * median_depth_;
                maquette_centre_[2] = head[2] + direction[2] * median_depth_;
            }
            // The scene-space point that sits at C0 now, so place_maquette()
            // writes the offset directly and a return to s = 1 restores the
            // captured offset exactly.
            const float start = state_.world_scale();
            const float* offset = state_.scene_offset();
            for (int axis = 0; axis < 3; ++axis) {
                maquette_anchor_[axis] = (maquette_centre_[axis] - offset[axis]) / start;
            }
            // s_m is a property of the scene, measured at scale 1: the robust
            // extent of the cloud itself, never of the currently displayed one.
            maquette_extent_ = robust_scene_extent(pivot_samples_.data(),
                                                   pivot_samples_.size() / 3, 1.0f);
            maquette_scale_ = maquette_scale_for_extent(maquette_extent_);
            maquette_captured_ = true;
        }
        // The marker is at T, whether or not the maquette has reached it: the
        // point the scene lands on and then scales about.
        for (int axis = 0; axis < 3; ++axis) pivot_world_[axis] = maquette_table_[axis];
        const float dx = maquette_table_[0] - head[0];
        const float dy = maquette_table_[1] - head[1];
        const float dz = maquette_table_[2] - head[2];
        pivot_distance_ = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    if (pivot_mode_ == ScalePivotMode::centre && cone_near_ > 0.0f) {
        if (distance <= kScaleNearGuardMetres || cone_near_ <= kScaleNearGuardMetres) {
            // The pivot, or the nearest content, is already at (or inside) the
            // 0.5 m guard - a tabletop scene. Any growth would push content
            // closer still, so this press may only shrink.
            centre_growth_limit_ = 1.0f;
        } else if (distance > cone_near_) {
            // nearest depth after a total factor g is D + g*(near - D); keep it
            // at or beyond kScaleNearGuardMetres. Stored as the largest total
            // factor.
            const float guard =
                (distance - kScaleNearGuardMetres) / (distance - cone_near_);
            centre_growth_limit_ = std::max(1.0f, guard);
        }
    }
    scale_gesture_ = true;
}

float HmdView::cone_pivot_distance(float* cone_near) const {
    if (cone_near != nullptr) *cone_near = 0.0f;
    if (pivot_samples_.size() / 3 == 0) return 0.0f;

    float head[3];
    placement_head(head);
    float forward[3];
    view_forward(forward);
    std::vector<float> depths;
    const size_t count =
        collect_cone_samples(pivot_samples_, head, forward, state_.world_scale(),
                             state_.scene_offset(), nullptr, nullptr, nullptr, &depths);
    if (count == 0) return 0.0f;

    auto percentile = [&depths](float percent) {
        std::vector<float> copy = depths;
        const size_t index = std::min(copy.size() - 1,
                                      static_cast<size_t>(percent * 0.01f *
                                                          static_cast<float>(copy.size())));
        std::nth_element(copy.begin(), copy.begin() + static_cast<long>(index), copy.end());
        return copy[index];
    };
    const float p5 = percentile(kScaleFrontPercentile);
    const float p50 = percentile(kScaleCentrePercentile);
    if (cone_near != nullptr) {
        *cone_near = std::clamp(p5, kScalePivotMinMetres, kScaleCentreMaxMetres);
    }
    if (pivot_mode_ == ScalePivotMode::centre) {
        // std::clamp requires lo <= hi; centre_pivot_bounds() caps the lower
        // bound at the ceiling for a distant scene (p5 > 60 m).
        float lower = kScalePivotMinMetres;
        float upper = kScaleCentreMaxMetres;
        centre_pivot_bounds(p5, &lower, &upper);
        return std::clamp(p50, lower, upper);
    }
    return std::clamp(p5, kScalePivotMinMetres, kScalePivotMaxMetres);
}

bool HmdView::cone_content_centre(float out[3]) const {
    float head[3];
    placement_head(head);
    float forward[3];
    view_forward(forward);
    std::vector<float> xs;
    std::vector<float> ys;
    std::vector<float> zs;
    const size_t count =
        collect_cone_samples(pivot_samples_, head, forward, state_.world_scale(),
                             state_.scene_offset(), &xs, &ys, &zs, nullptr);
    if (count == 0) return false;
    // Per-axis median: a robust centre of the visible cone (the mean would be
    // dragged by a few far splats). It need not be a splat itself.
    auto median = [](std::vector<float>& values) {
        const size_t middle = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + static_cast<long>(middle), values.end());
        return values[middle];
    };
    out[0] = median(xs);
    out[1] = median(ys);
    out[2] = median(zs);
    return true;
}

float HmdView::place_maquette(float scale) {
    // The scene-space anchor is where the content centre was at the press's
    // start. Writing the offset directly (rather than compensating a pivot)
    // keeps the whole glide and the table scaling one continuous function of
    // the absolute scale, so growing back reaches C0 and the original offset
    // again.
    state_.set_world_scale(scale);
    const float applied = state_.world_scale();
    float centre[3];
    maquette_centre(applied, maquette_centre_, maquette_table_, maquette_scale_, centre);
    state_.set_scene_offset(centre[0] - applied * maquette_anchor_[0],
                            centre[1] - applied * maquette_anchor_[1],
                            centre[2] - applied * maquette_anchor_[2]);
    return applied;
}

void HmdView::view_forward(float out[3]) const {
    // The renderer's camera forward for view_yaw/scene_pitch (splat_renderer):
    // f = (sin yaw cos pitch, sin pitch, cos yaw cos pitch), scene y down.
    const double yaw = view_yaw();
    const double pitch = scene_pitch_;
    const float cp = static_cast<float>(std::cos(pitch));
    out[0] = static_cast<float>(std::sin(yaw)) * cp;
    out[1] = static_cast<float>(std::sin(pitch));
    out[2] = static_cast<float>(std::cos(yaw)) * cp;
}

void HmdView::scale_scene(float factor, double seconds) {
    if (!(factor > 0.0f) || !std::isfinite(factor)) return;
    // Visible scale without flying: the scene grows or shrinks about a point
    // fixed in the world ahead (begin_scale_gesture()), so that point keeps
    // its place in the picture, the viewer stays exactly where they are and
    // the scene visibly changes around them. VR6 scaled about the capture
    // point at the eyes, which is a fixed point of the scene - the image was
    // the same at any scale ("scale does absolutely nothing"); VR5 compensated
    // a pivot 25-50 m away, which moved the camera by up to 139 m in one press
    // (runs/vr5/REPORT.md, runs/vr6/REPORT.md).
    //
    // The compensation is a pure translation in the placement frame, folded
    // into the scene offset, so it composes with a lift, a move or a turn in
    // any order: offset' = pivot + f * (offset - pivot) is the same formula a
    // rigid transform plus uniform scale has to obey.
    if (!scale_gesture_) begin_scale_gesture();

    if (pivot_mode_ == ScalePivotMode::maquette) {
        // VR9: the scene shrinks and lands on the table, then grows or shrinks
        // in place there. The placement is a function of the absolute scale
        // (place_maquette), not an accumulating pivot compensation, so the
        // round trip back to 1 returns the original centre and offset exactly,
        // and s_m is a single continuous join between glide and table scaling.
        const float current = state_.world_scale();
        float target = std::max(kMinWorldScale, std::min(kMaxWorldScale, current * factor));
        if (seconds > 0.0) {
            // The dolly-style speed limit still applies to the glide: cap how
            // far the content centre may move this frame.
            float now_centre[3];
            float next_centre[3];
            maquette_centre(current, maquette_centre_, maquette_table_, maquette_scale_, now_centre);
            maquette_centre(target, maquette_centre_, maquette_table_, maquette_scale_, next_centre);
            float dx = next_centre[0] - now_centre[0];
            float dy = next_centre[1] - now_centre[1];
            float dz = next_centre[2] - now_centre[2];
            const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float max_moved = kScaleDollyMetresPerSecond * static_cast<float>(seconds);
            if (moved > max_moved && moved > 1.0e-6f) {
                const float keep = max_moved / moved;
                target = current + (target - current) * keep;
            }
        }
        if (target == current) return;
        place_maquette(target);
        return;
    }

    // The dolly term of the transform is (1 - f) * D metres. Cap its speed
    // with the measured frame dt so a long press at a distant pivot cannot
    // push the scene through the user faster than the research's 1.5 m/s.
    if (seconds > 0.0 && pivot_distance_ > 1.0e-4) {
        const float max_delta = kScaleDollyMetresPerSecond *
                                static_cast<float>(seconds) / pivot_distance_;
        if (factor > 1.0f) {
            factor = std::min(factor, 1.0f + max_delta);
        } else if (factor < 1.0f) {
            factor = std::max(factor, 1.0f - max_delta);
        }
    }
    // Centre mode: stop growing before the nearest content in the cone reaches
    // kScaleNearGuardMetres. `centre_growth_limit_` is the largest total factor
    // this press may reach; scaling down is always allowed.
    if (centre_growth_limit_ > 0.0f && factor > 1.0f) {
        const float current =
            gesture_start_scale_ > 0.0f ? state_.world_scale() / gesture_start_scale_ : 1.0f;
        const float room = centre_growth_limit_ / std::max(current, 1.0e-6f);
        if (factor > room) factor = std::max(1.0f, room);
    }
    if (factor == 1.0f) return;
    state_.scale_about_pivot(factor, pivot_world_);
}

// ---- PanelFollower ---------------------------------------------------------

namespace {

void normalise3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1.0e-6f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

// Camera forward from yaw/pitch with the pitch clamped just short of vertical:
// at +-90 deg the panel's up direction is undefined and the rectangle would
// collapse.
void panel_forward(double yaw, double pitch, float out[3]) {
    constexpr double kMaxPitch = 80.0 * 3.14159265358979323846 / 180.0;
    const double clamped = std::max(-kMaxPitch, std::min(kMaxPitch, pitch));
    const float cp = static_cast<float>(std::cos(clamped));
    out[0] = static_cast<float>(std::sin(yaw)) * cp;
    out[1] = static_cast<float>(std::sin(clamped));
    out[2] = static_cast<float>(std::cos(yaw)) * cp;
}

}  // namespace

void PanelFollower::reset(double yaw, double pitch) {
    panel_forward(yaw, pitch, direction_);
    initialised_ = true;
    following_ = false;
}

void PanelFollower::update(double yaw, double pitch, double seconds) {
    if (!initialised_) {
        reset(yaw, pitch);
        return;
    }
    float target[3];
    panel_forward(yaw, pitch, target);
    const float dot = direction_[0] * target[0] + direction_[1] * target[1] +
                      direction_[2] * target[2];
    const float angle = std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 180.0f /
                        3.14159265358979323846f;
    if (angle > kPanelFollowStartDegrees) following_ = true;
    if (!following_ || !(seconds > 0.0)) return;
    const float alpha =
        1.0f - static_cast<float>(std::exp(-seconds / kPanelFollowSeconds));
    for (int axis = 0; axis < 3; ++axis) {
        direction_[axis] += (target[axis] - direction_[axis]) * alpha;
    }
    normalise3(direction_);
    const float settled = direction_[0] * target[0] + direction_[1] * target[1] +
                          direction_[2] * target[2];
    if (std::acos(std::max(-1.0f, std::min(1.0f, settled))) * 180.0f /
            3.14159265358979323846f <
        kPanelFollowSettleDegrees) {
        for (int axis = 0; axis < 3; ++axis) direction_[axis] = target[axis];
        following_ = false;
    }
}

OverlayQuad PanelFollower::quad(const float eye[3], double yaw, double pitch) const {
    float forward[3];
    panel_forward(yaw, pitch, forward);
    // Upright without roll: world up (-y in scene axes) made perpendicular to
    // the view direction. Right x up = -forward, so the panel's front faces the
    // viewer; a parallel plane projects to a rigid rectangle whatever the
    // head's roll.
    float up[3] = {0.0f, -1.0f, 0.0f};
    const float along = up[0] * forward[0] + up[1] * forward[1] + up[2] * forward[2];
    for (int axis = 0; axis < 3; ++axis) up[axis] -= along * forward[axis];
    normalise3(up);
    float right[3] = {forward[1] * up[2] - forward[2] * up[1],
                      forward[2] * up[0] - forward[0] * up[2],
                      forward[0] * up[1] - forward[1] * up[0]};
    normalise3(right);

    OverlayQuad quad;
    for (int axis = 0; axis < 3; ++axis) {
        quad.center[axis] = eye[axis] + direction_[axis] * distance;
        quad.right[axis] = right[axis];
        quad.up[axis] = up[axis];
    }
    quad.width = 1.4f;
    quad.height = 0.7875f;
    return quad;
}

void HmdView::reset_scene_adjust() {
    state_.set_world_scale(initial_scale_);
    state_.set_scene_offset(0.0f, 0.0f, 0.0f);
    turn_ = 0.0f;
    // A pivot frozen before the re-anchor is a point in the old placement
    // frame; a press that continues across a recentre picks a new one.
    scale_gesture_ = false;
    // The maquette round trip ends with the re-anchor: the next press captures
    // a fresh T/C0/s_m for the new placement.
    maquette_captured_ = false;
}

void HmdView::turn_scene(float radians) {
    if (!std::isfinite(radians)) return;
    // Keep the accumulated angle in [-pi, pi]: the frame only needs the angle
    // mod a full turn, and a long session must not lose precision in it.
    constexpr float kTwoPi = 6.283185307179586f;
    turn_ = std::remainder(turn_ + radians, kTwoPi);
}

void HmdView::move_viewer(float right, float forward, float up) {
    // Locomotion: moving the viewer right/forward/up is moving the scene the
    // other way (ViewerState's frame subtracts the offset).
    move_scene(-right, -forward, -up);
}

void HmdView::turn_viewer(float radians) {
    // Turning the viewer to their right is turning the scene to their left;
    // turn_scene()'s positive direction is the scene's.
    turn_scene(-radians);
}

void HmdView::move_scene(float right, float forward, float up) {
    // The head's yaw in the placement frame, with pitch dropped: the sticks
    // move the scene along the floor, not along the view direction.
    const float fx = dot3(placement_right_, head_forward_);
    const float fz = dot3(placement_forward_, head_forward_);
    const float length = std::sqrt(fx * fx + fz * fz);
    const float yaw_x = length > 1.0e-4f ? fx / length : 0.0f;
    const float yaw_z = length > 1.0e-4f ? fz / length : 1.0f;
    // Forward (sin, 0, cos) and the camera's own level right (cos, 0, -sin),
    // the same pair the renderer builds its view from.
    state_.move_scene(yaw_z * right + yaw_x * forward, -up, -yaw_x * right + yaw_z * forward);
}

void HmdView::camera_position(float out[3]) const {
    // The scene moved by `offset` is the camera moved the other way: the head
    // in the placement frame minus the scene offset is the camera, for each
    // axis (rotated by the placement rotation first).
    placement_head(out);
    for (int axis = 0; axis < 3; ++axis) out[axis] -= state_.scene_offset()[axis];
}

void HmdView::placement_head(float out[3]) const {
    const float offset[3] = {head_[0] - state_.anchor()[0], head_[1] - state_.anchor()[1],
                             head_[2] - state_.anchor()[2]};
    out[0] = dot3(placement_right_, offset);
    out[1] = dot3(placement_down_, offset);
    out[2] = dot3(placement_forward_, offset);
}

bool HmdView::update(const HeadPoses& poses) {
    if (!poses.head.tracked) return false;
    float eye_mid[3] = {poses.head.position[0], poses.head.position[1], poses.head.position[2]};
    if (poses.left.tracked && poses.right.tracked) {
        for (int axis = 0; axis < 3; axis++)
            eye_mid[axis] = 0.5f * (poses.left.position[axis] + poses.right.position[axis]);
        // IPD along the head's own x axis, which is the axis the renderer moves
        // each eye along (splat.vert's eye_offset). HLA-PSVR2 uses the two
        // tracked eye poses themselves (psvr2.c:705-712, xr_session.c:571-574);
        // their separation along the head's right axis is what that pair's
        // screen shift amounts to, and hmd2 has already rejected an implausible
        // pair (2-12 cm).
        float right[3];
        hmd_basis(poses.head.quaternion, right, nullptr, nullptr);
        const float tracker_separation[3] = {poses.right.position[0] - poses.left.position[0],
                                             poses.right.position[1] - poses.left.position[1],
                                             poses.right.position[2] - poses.left.position[2]};
        float scene_separation[3];
        hmd_scene_position(tracker_separation, scene_separation);
        const float separation = right[0] * scene_separation[0] +
                                 right[1] * scene_separation[1] +
                                 right[2] * scene_separation[2];
        if (separation >= 0.02f && separation <= 0.12f) ipd_ = separation;
    }
    hmd_scene_position(eye_mid, head_);
    hmd_basis(poses.head.quaternion, head_right_, head_down_, head_forward_);
    decompose_axes(head_right_, head_forward_, &head_yaw_, &head_pitch_, &head_roll_);
    update_scene_angles();
    return true;
}

void HmdView::update_scene_angles() {
    // The head's axes in the scene frame: R_placement^T * R_head, i.e. each
    // head axis resolved on the placement axes. Until the first place() the
    // placement rotation is the identity, so these are the head's own angles.
    float right[3] = {dot3(placement_right_, head_right_), dot3(placement_down_, head_right_),
                      dot3(placement_forward_, head_right_)};
    float forward[3] = {dot3(placement_right_, head_forward_),
                        dot3(placement_down_, head_forward_),
                        dot3(placement_forward_, head_forward_)};
    decompose_axes(right, forward, &scene_yaw_, &scene_pitch_, &scene_roll_);
}

bool HmdView::place(const HeadPoses& poses) {
    if (!update(poses)) return false;
    recentre_here();
    return true;
}

void HmdView::recentre_here() {
    state_.recentre(head_, static_cast<float>(head_yaw_));
    // The head's own frame becomes the scene's frame: yaw zero, level and
    // upright, so the photo is in front rather than wherever the head happened
    // to point. This is the whole placement transform - the scene is the photo
    // seen from this pose - so the pose handed to Hmd2 stays the tracker pose.
    for (int axis = 0; axis < 3; axis++) {
        placement_right_[axis] = head_right_[axis];
        placement_down_[axis] = head_down_[axis];
        placement_forward_[axis] = head_forward_[axis];
    }
    // The placement frame moved: a pivot frozen in the old one no longer means
    // anything.
    scale_gesture_ = false;
    update_scene_angles();
}

FrameView HmdView::frame() const {
    FrameView view;
    // One rigid transform for all six degrees of freedom: the head's position
    // and orientation expressed in the placement frame. Rotating the position
    // by the same rotation as the orientation is what keeps a step in the
    // head's x a step in the camera's x, and so the parallax direction right.
    // The scene offset is the "move the scene" adjustment: the scene moved by
    // `offset` is the camera moved the other way.
    camera_position(view.midpoint.position);
    view.midpoint.yaw = static_cast<float>(view_yaw());
    view.midpoint.pitch = static_cast<float>(scene_pitch_);
    // The roll the yaw/pitch pair cannot carry: rolling the head must roll the
    // image the other way, not leave it level with the head.
    view.midpoint.roll = static_cast<float>(scene_roll_);
    view.ipd = ipd_;
    view.scene_scale = state_.world_scale();
    view.comfort_fade = state_.comfort_fade(head_);
    return view;
}

}  // namespace splat
