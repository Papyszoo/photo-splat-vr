// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pointer.h"

#include <algorithm>
#include <cmath>

namespace splat {
namespace {

constexpr float kEpsilon = 1e-4f;

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// q * v * q^-1, for a unit quaternion (x, y, z, w): the vector v turned by q.
void rotate(const float q[4], const float v[3], float out[3]) {
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    // t = 2 * (q.xyz x v); v' = v + qw * t + q.xyz x t.
    const float tx = 2.0f * (qy * v[2] - qz * v[1]);
    const float ty = 2.0f * (qz * v[0] - qx * v[2]);
    const float tz = 2.0f * (qx * v[1] - qy * v[0]);
    out[0] = v[0] + qw * tx + (qy * tz - qz * ty);
    out[1] = v[1] + qw * ty + (qz * tx - qx * tz);
    out[2] = v[2] + qw * tz + (qx * ty - qy * tx);
}

void normalize_quaternion(float q[4]) {
    const float length =
        std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0.0f) || !std::isfinite(length)) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (int i = 0; i < 4; ++i) q[i] /= length;
}

// The head's scene axes (render/ui_panel.cpp, ui_host.cpp view_from_pose):
// yaw about scene y, pitch about the yawed x, roll about the pitched forward
// axis. `right`/`down`/`forward` are unit vectors in scene coordinates, so a
// head-local OpenCV vector (x right, y down, z forward) maps to
// x*right + y*down + z*forward.
void scene_basis(const CameraPose& pose, float right[3], float down[3], float forward[3]) {
    const float cy = std::cos(pose.yaw);
    const float sy = std::sin(pose.yaw);
    const float cp = std::cos(pose.pitch);
    const float sp = std::sin(pose.pitch);
    forward[0] = sy * cp;
    forward[1] = sp;
    forward[2] = cy * cp;
    right[0] = cy;
    right[1] = 0.0f;
    right[2] = -sy;
    // down = forward x right, the right-handed OpenCV basis at zero roll.
    down[0] = forward[1] * right[2] - forward[2] * right[1];
    down[1] = forward[2] * right[0] - forward[0] * right[2];
    down[2] = forward[0] * right[1] - forward[1] * right[0];

    const float cr = std::cos(pose.roll);
    const float sr = std::sin(pose.roll);
    const float level_right[3] = {right[0], right[1], right[2]};
    const float level_down[3] = {down[0], down[1], down[2]};
    for (int axis = 0; axis < 3; ++axis) {
        right[axis] = cr * level_right[axis] + sr * level_down[axis];
        down[axis] = cr * level_down[axis] - sr * level_right[axis];
    }
}

}  // namespace

std::optional<PanelHit> ray_panel_hit(const float origin[3], const float dir[3],
                                      const OverlayQuad& panel, uint32_t canvas_w,
                                      uint32_t canvas_h) {
    if (canvas_w == 0 || canvas_h == 0 || origin == nullptr || dir == nullptr) return std::nullopt;
    if (!(panel.width > 0.0f) || !(panel.height > 0.0f)) return std::nullopt;

    float normal[3];
    cross3(panel.right, panel.up, normal);
    const float denominator = dot3(dir, normal);
    if (std::fabs(denominator) < kEpsilon) return std::nullopt;  // parallel to the panel

    float to_panel[3] = {panel.center[0] - origin[0], panel.center[1] - origin[1],
                         panel.center[2] - origin[2]};
    const float t = dot3(to_panel, normal) / denominator;
    if (!(t > 0.0f) || !std::isfinite(t)) return std::nullopt;

    float local[3] = {origin[0] + t * dir[0] - panel.center[0],
                      origin[1] + t * dir[1] - panel.center[1],
                      origin[2] + t * dir[2] - panel.center[2]};
    // The quad's axes: right is +u, up points to the texture's top edge and
    // canvas y grows downwards, so a point above the centre is a smaller y.
    const float u = dot3(local, panel.right) / panel.width + 0.5f;
    const float v = 0.5f - dot3(local, panel.up) / panel.height;
    if (!(u >= -kEpsilon) || !(u <= 1.0f + kEpsilon) || !(v >= -kEpsilon) ||
        !(v <= 1.0f + kEpsilon)) {
        return std::nullopt;
    }

    const float x = std::min(1.0f, std::max(0.0f, u)) * static_cast<float>(canvas_w);
    const float y = std::min(1.0f, std::max(0.0f, v)) * static_cast<float>(canvas_h);
    return PanelHit{x, y};
}

PointerRay controller_ray(const float head_pos[3], const float head_quat[4],
                          const float controller_pos[3], const float controller_quat[4],
                          const CameraPose& head_scene) {
    PointerRay ray;
    float head[4] = {head_quat[0], head_quat[1], head_quat[2], head_quat[3]};
    float controller[4] = {controller_quat[0], controller_quat[1], controller_quat[2],
                           controller_quat[3]};
    normalize_quaternion(head);
    normalize_quaternion(controller);

    // Head-local tracker coordinates of the controller: position relative to
    // the head, turned back by the head's inverse orientation. q^-1 = conj(q).
    const float inverse_head[4] = {-head[0], -head[1], -head[2], head[3]};
    const float relative_position[3] = {controller_pos[0] - head_pos[0],
                                        controller_pos[1] - head_pos[1],
                                        controller_pos[2] - head_pos[2]};
    float local_position[3];
    rotate(inverse_head, relative_position, local_position);

    // Tracker axes to the head's OpenCV axes: +y up becomes +y down, -z forward
    // becomes +z forward (hmd_view.h's hmd_basis, same rotation).
    const float scene_position[3] = {local_position[0], -local_position[1],
                                     -local_position[2]};

    // The controller's forward axis relative to the head: local -z, turned by
    // q_head^-1 * q_controller, then the same axis swap.
    const float local_forward[3] = {0.0f, 0.0f, -1.0f};
    float in_tracker[3];
    rotate(controller, local_forward, in_tracker);
    float relative_forward[3];
    rotate(inverse_head, in_tracker, relative_forward);
    const float forward[3] = {relative_forward[0], -relative_forward[1], -relative_forward[2]};

    float right[3], down[3], scene_forward[3];
    scene_basis(head_scene, right, down, scene_forward);
    for (int axis = 0; axis < 3; ++axis) {
        ray.origin[axis] = head_scene.position[axis] + scene_position[0] * right[axis] +
                           scene_position[1] * down[axis] + scene_position[2] * scene_forward[axis];
        ray.direction[axis] =
            forward[0] * right[axis] + forward[1] * down[axis] + forward[2] * scene_forward[axis];
    }
    const float length = std::sqrt(dot3(ray.direction, ray.direction));
    if (length > kEpsilon) {
        for (int axis = 0; axis < 3; ++axis) ray.direction[axis] /= length;
    } else {
        ray.direction[0] = 0.0f;
        ray.direction[1] = 0.0f;
        ray.direction[2] = 1.0f;
    }
    return ray;
}

PointerFilter::PointerFilter(float time_constant_seconds) {
    time_constant_ = time_constant_seconds > 0.0f ? time_constant_seconds : 0.08f;
}

std::optional<PanelHit> PointerFilter::update(const std::optional<PanelHit>& hit,
                                              double dt_seconds) {
    if (!hit) {
        reset();
        return std::nullopt;
    }
    if (!valid_) {
        x_ = hit->x;
        y_ = hit->y;
        valid_ = true;
        return hit;
    }
    // alpha = 1 - exp(-dt / tau): a fixed fraction per elapsed time, so the
    // same input gives the same output regardless of frame rate or wall clock.
    const double dt = dt_seconds > 0.0 ? dt_seconds : 0.0;
    const float alpha =
        1.0f - std::exp(-static_cast<float>(dt) / time_constant_);
    x_ += (hit->x - x_) * std::min(1.0f, std::max(0.0f, alpha));
    y_ += (hit->y - y_) * std::min(1.0f, std::max(0.0f, alpha));
    return PanelHit{x_, y_};
}

void PointerFilter::reset() {
    valid_ = false;
    x_ = 0.0f;
    y_ = 0.0f;
}

}  // namespace splat
