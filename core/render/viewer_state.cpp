#include "render/viewer_state.h"

#include <algorithm>
#include <cmath>

namespace splat {
namespace {

float clamp(float value, float low, float high) { return std::max(low, std::min(high, value)); }

// 3t^2 - 2t^3 on [0, 1].
float smoothstep(float t) {
    t = clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

ViewerState::ViewerState() = default;

void ViewerState::set_anchor(float x, float y, float z) {
    anchor_[0] = x;
    anchor_[1] = y;
    anchor_[2] = z;
}

void ViewerState::set_world_scale(float scale) {
    world_scale_ = clamp(scale, kMinWorldScale, kMaxWorldScale);
}

void ViewerState::set_scene_offset(float x, float y, float z) {
    offset_[0] = x;
    offset_[1] = y;
    offset_[2] = z;
}

void ViewerState::scale_about_pivot(float factor, const float pivot[3]) {
    if (!(factor > 0.0f) || !std::isfinite(factor) || pivot == nullptr) return;
    const float before = world_scale_;
    set_world_scale(before * factor);
    // The scale range clamps, so compensate by what was actually applied: at the
    // 4x limit a press must leave the scene where it is, not keep sliding it
    // towards the pivot at the rate the trigger asked for.
    const float applied = world_scale_ / before;
    if (!(applied > 0.0f) || !std::isfinite(applied)) return;
    for (int axis = 0; axis < 3; ++axis) {
        offset_[axis] = pivot[axis] + applied * (offset_[axis] - pivot[axis]);
    }
}

void ViewerState::set_comfort_radius(float radius) {
    comfort_radius_ = clamp(radius, kMinComfortRadius, kMaxComfortRadius);
}

void ViewerState::set_fade_start(float fraction) {
    fade_start_ = clamp(fraction, 0.0f, 1.0f);
}

void ViewerState::recentre(const float head_position[3], float head_yaw) {
    // The capture point moves under the head, and the head's current yaw
    // becomes the scene's zero yaw. Pitch is unchanged.
    set_anchor(head_position[0], head_position[1], head_position[2]);
    yaw_offset_ = head_yaw;
}

CameraPose ViewerState::scene_pose(const float head_position[3], float head_yaw,
                                   float head_pitch) const {
    const float dx = head_position[0] - anchor_[0];
    const float dy = head_position[1] - anchor_[1];
    const float dz = head_position[2] - anchor_[2];

    // Rotate the tracking offset into the scene frame by -yaw_offset_. The
    // camera right/forward axes for yaw a are (cos a, 0, -sin a) and
    // (sin a, 0, cos a), so the scene offset is (dot(offset, right),
    // offset.y, dot(offset, forward)).
    const float c = std::cos(yaw_offset_);
    const float s = std::sin(yaw_offset_);
    CameraPose pose;
    // The scene offset is the user's "move the scene" adjustment: the scene
    // moved by `offset` is the camera moved by -offset.
    pose.position[0] = c * dx - s * dz - offset_[0];
    pose.position[1] = dy - offset_[1];
    pose.position[2] = s * dx + c * dz - offset_[2];
    pose.yaw = head_yaw - yaw_offset_;
    pose.pitch = head_pitch;
    return pose;
}

float ViewerState::head_distance(const float head_position[3]) const {
    const float dx = head_position[0] - anchor_[0];
    const float dy = head_position[1] - anchor_[1];
    const float dz = head_position[2] - anchor_[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float ViewerState::fade_for_distance(float distance) const {
    if (!(comfort_radius_ > 0.0f)) return distance <= 0.0f ? 1.0f : 0.0f;
    const float start = fade_start_ * comfort_radius_;
    if (distance <= start) return 1.0f;
    if (distance >= comfort_radius_) return 0.0f;
    const float span = comfort_radius_ - start;
    if (!(span > 0.0f)) return 0.0f;
    return 1.0f - smoothstep((distance - start) / span);
}

float ViewerState::comfort_fade(const float head_position[3]) const {
    return fade_for_distance(head_distance(head_position));
}

FrameView ViewerState::frame(const float head_position[3], float head_yaw, float head_pitch,
                             float ipd) const {
    FrameView view;
    view.midpoint = scene_pose(head_position, head_yaw, head_pitch);
    view.ipd = ipd;
    view.scene_scale = world_scale_;
    view.comfort_fade = comfort_fade(head_position);
    return view;
}

}  // namespace splat
