// VR8: scale pivots, the lazy VR panel and the persisted comfort/display
// values. These are CPU-only (no Vulkan): the same HmdView and PanelFollower
// code the console runs, driven with synthetic poses and a synthetic splat
// cloud, so the geometry is checked on any desktop build.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "render/hmd_view.h"
#include "ui/ui_state.h"
#include "../platform/ps5/viewer/overlay_refresh.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

splat::HeadPoses identity_head() {
    splat::HeadPoses poses;
    for (splat::TrackedPose* pose : {&poses.head, &poses.left, &poses.right}) {
        pose->tracked = true;
        pose->position[0] = pose->position[1] = pose->position[2] = 0.0f;
        pose->quaternion[0] = pose->quaternion[1] = pose->quaternion[2] = 0.0f;
        pose->quaternion[3] = 1.0f;
    }
    return poses;
}

// `near_count` points at `near_z` and the rest at `far_z`, all inside a narrow
// cone around +z (so the 15 deg cone keeps every one).
std::vector<float> two_depth_cloud(float near_z, int near_count, float far_z, int far_count) {
    std::vector<float> points;
    points.reserve(static_cast<size_t>(near_count + far_count) * 3);
    for (int i = 0; i < near_count; ++i) {
        const float angle = static_cast<float>(i) * 0.01f;
        points.push_back(0.05f * std::cos(angle));
        points.push_back(0.05f * std::sin(angle));
        points.push_back(near_z);
    }
    for (int i = 0; i < far_count; ++i) {
        const float angle = static_cast<float>(i) * 0.01f;
        points.push_back(0.10f * std::cos(angle));
        points.push_back(0.10f * std::sin(angle));
        points.push_back(far_z);
    }
    return points;
}

// The renderer's world -> camera matrix for a CameraPose (splat_renderer.cpp).
void view_from_pose(const splat::CameraPose& pose, double view[16]) {
    const double cy = std::cos(pose.yaw), sy = std::sin(pose.yaw);
    const double cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const double f[3] = {sy * cp, sp, cy * cp};
    double s[3] = {cy, 0.0, -sy};
    double d[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
                   f[0] * s[1] - f[1] * s[0]};
    const double cr = std::cos(pose.roll), sr = std::sin(pose.roll);
    const double s0[3] = {s[0], s[1], s[2]};
    const double d0[3] = {d[0], d[1], d[2]};
    for (int axis = 0; axis < 3; ++axis) {
        s[axis] = cr * s0[axis] + sr * d0[axis];
        d[axis] = cr * d0[axis] - sr * s0[axis];
    }
    const float* p = pose.position;
    view[0] = s[0];
    view[1] = d[0];
    view[2] = f[0];
    view[3] = 0.0;
    view[4] = s[1];
    view[5] = d[1];
    view[6] = f[1];
    view[7] = 0.0;
    view[8] = s[2];
    view[9] = d[2];
    view[10] = f[2];
    view[11] = 0.0;
    view[12] = -(s[0] * p[0] + s[1] * p[1] + s[2] * p[2]);
    view[13] = -(d[0] * p[0] + d[1] * p[1] + d[2] * p[2]);
    view[14] = -(f[0] * p[0] + f[1] * p[1] + f[2] * p[2]);
    view[15] = 1.0;
}

struct Pixel {
    double x = 0.0;
    double y = 0.0;
};

// Projects a world point with the renderer's camera and a 900 px focal length.
Pixel project(const splat::CameraPose& pose, const float world[3]) {
    double view[16];
    view_from_pose(pose, view);
    const double camera[3] = {
        view[0] * world[0] + view[4] * world[1] + view[8] * world[2] + view[12],
        view[1] * world[0] + view[5] * world[1] + view[9] * world[2] + view[13],
        view[2] * world[0] + view[6] * world[1] + view[10] * world[2] + view[14]};
    REQUIRE(camera[2] > 0.0);
    return {900.0 * camera[0] / camera[2] + 640.0, 900.0 * camera[1] / camera[2] + 360.0};
}

double distance(const Pixel& a, const Pixel& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

}  // namespace

TEST_CASE("vr8 front pivot is the 5th-percentile depth in the head cone") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::front);
    // 100 points at 0.8 m, 900 at 4 m: p5 lands in the near group, p50 in the
    // far one. All are inside the cone.
    view.set_pivot_samples(two_depth_cloud(0.8f, 100, 4.0f, 900));
    view.begin_scale_gesture();
    CHECK(view.scale_pivot_distance() == doctest::Approx(0.8f).epsilon(0.02));
    CHECK(view.scale_pivot_world()[2] ==
          doctest::Approx(view.scale_pivot_distance()).epsilon(1e-5));
    CHECK(view.scale_cone_near() == doctest::Approx(0.8f).epsilon(0.02));
}

TEST_CASE("vr8 centre pivot is the cone median, with a 0.5 m near-content guard") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::centre);
    // p5 = 1 m, p50 = 6 m; growing must stop before the 1 m content reaches
    // 0.5 m: D + g*(near - D) = 6 - 5g >= 0.5 -> g <= 1.1.
    view.set_pivot_samples(two_depth_cloud(1.0f, 100, 6.0f, 900));
    view.begin_scale_gesture();
    CHECK(view.scale_pivot_distance() == doctest::Approx(6.0f).epsilon(0.02));
    CHECK(view.world_scale() == doctest::Approx(1.0f));
    view.scale_scene(2.0f);  // dt 0: the guard alone decides
    CHECK(view.world_scale() == doctest::Approx(1.1f).epsilon(0.01));
    view.scale_scene(2.0f);
    CHECK(view.world_scale() == doctest::Approx(1.1f).epsilon(0.01));
    // Shrinking is still allowed.
    view.scale_scene(0.5f);
    CHECK(view.world_scale() < 1.1f);
}

TEST_CASE("vr8 centre pivot stays inside the clamp when the cone is beyond 60 m") {
    // std::clamp needs lo <= hi: p5 = 70 m used to make the lower bound 70 and
    // the upper 60, which is undefined behaviour (an abort under hardened
    // standard libraries). The bound helper keeps them ordered.
    float lower = 0.0f;
    float upper = 0.0f;
    splat::centre_pivot_bounds(70.0f, &lower, &upper);
    CHECK(lower <= upper);
    CHECK(lower == doctest::Approx(splat::kScaleCentreMaxMetres));
    CHECK(upper == doctest::Approx(splat::kScaleCentreMaxMetres));
    // The normal case keeps p5 as the lower bound, and the 0.3 m floor still
    // applies to a very near p5.
    splat::centre_pivot_bounds(4.0f, &lower, &upper);
    CHECK(lower == doctest::Approx(4.0f));
    splat::centre_pivot_bounds(0.1f, &lower, &upper);
    CHECK(lower == doctest::Approx(splat::kScalePivotMinMetres));

    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::centre);
    // A distant scene: p5 = 70 m, p50 = 85 m. The press must still run, with
    // the pivot clamped to the 60 m ceiling.
    view.set_pivot_samples(two_depth_cloud(70.0f, 100, 85.0f, 900));
    view.begin_scale_gesture();
    CHECK(view.scale_pivot_distance() == doctest::Approx(60.0f));
    CHECK(view.scale_cone_near() == doctest::Approx(60.0f));
    view.scale_scene(1.5f);
    CHECK(view.world_scale() == doctest::Approx(1.5f).epsilon(1e-5));
}

TEST_CASE("vr8 centre scale keeps the 0.5 m near guard when the median is under 0.5 m") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::centre);
    // Tabletop: p5 = 0.3 m, p50 = 0.45 m - the pivot is already inside the
    // guard, so growth must be blocked outright rather than left unconstrained.
    view.set_pivot_samples(two_depth_cloud(0.3f, 100, 0.45f, 900));
    view.begin_scale_gesture();
    CHECK(view.scale_pivot_distance() == doctest::Approx(0.45f).epsilon(0.02));
    CHECK(view.scale_cone_near() == doctest::Approx(0.3f).epsilon(0.02));
    view.scale_scene(2.0f);  // dt 0: no growth is allowed
    CHECK(view.world_scale() == doctest::Approx(1.0f));
    // Shrinking is still allowed.
    view.scale_scene(0.5f);
    CHECK(view.world_scale() == doctest::Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("vr8 the overlay is rebuilt every frame the panel is visible") {
    viewer::OverlayRefresh idle;
    CHECK_FALSE(viewer::overlay_needs_update(idle));

    // The panel's lazy follower moves its quads every frame even though the
    // scene, HUD, marker and textures are unchanged: without this term the
    // drawn panel freezes at the pose the menu opened from.
    idle.panel_visible = true;
    CHECK(viewer::overlay_needs_update(idle));

    // The panel's open/close edge, the other elements and a scene move still
    // count on their own.
    viewer::OverlayRefresh edges;
    edges.panel_changed = true;
    CHECK(viewer::overlay_needs_update(edges));
    edges = {};
    edges.hud_shown = true;  // the frame the last HUD element disappears
    CHECK(viewer::overlay_needs_update(edges));
    edges = {};
    edges.marker_changed = true;
    CHECK(viewer::overlay_needs_update(edges));
    edges = {};
    edges.moved = true;
    CHECK(viewer::overlay_needs_update(edges));
    edges = {};
    edges.dirty = true;
    CHECK(viewer::overlay_needs_update(edges));
}

TEST_CASE("vr8 a scale press caps the dolly speed at 1.5 m/s") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::front);
    view.set_pivot_samples(two_depth_cloud(4.0f, 1000, 4.0f, 0));
    view.begin_scale_gesture();
    REQUIRE(view.scale_pivot_distance() == doctest::Approx(4.0f).epsilon(0.01));
    const double dt = 1.0 / 60.0;
    view.scale_scene(2.0f, dt);
    // (f - 1) * D <= 1.5 * dt.
    CHECK((view.world_scale() - 1.0f) * view.scale_pivot_distance() <=
          1.5f * static_cast<float>(dt) + 1.0e-5f);
    CHECK(view.world_scale() < 1.01f);
}

TEST_CASE("vr8 the panel follows lazily and keeps a fixed distance") {
    splat::PanelFollower panel;
    panel.reset(0.0, 0.0);
    CHECK(panel.direction()[2] == doctest::Approx(1.0f));
    // A small head turn is inside the dead zone: the panel stays put.
    panel.update(10.0 * kPi / 180.0, 0.0, 1.0 / 60.0);
    CHECK(panel.direction()[2] == doctest::Approx(1.0f));
    CHECK_FALSE(panel.following());
    // A large turn re-centres; after ~0.3 s it is back in front.
    const double yaw = 40.0 * kPi / 180.0;
    panel.update(yaw, 0.0, 0.1);
    CHECK(panel.following());
    for (int i = 0; i < 24; ++i) panel.update(yaw, 0.0, 1.0 / 60.0);
    CHECK(panel.direction()[0] == doctest::Approx(std::sin(yaw)).epsilon(0.03));
    CHECK_FALSE(panel.following());
    // The quad stays at the fixed distance, even after the re-centre.
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    const splat::OverlayQuad quad = panel.quad(eye, yaw, 0.0);
    const double length = std::sqrt(quad.center[0] * quad.center[0] +
                                    quad.center[1] * quad.center[1] +
                                    quad.center[2] * quad.center[2]);
    CHECK(length == doctest::Approx(splat::kPanelDistanceMetres).epsilon(1e-5));
}

TEST_CASE("vr8 a rolled and pitched head projects the panel as a rigid rectangle") {
    splat::PanelFollower panel;
    panel.reset(0.0, 0.0);
    // The head has a yaw, a pitch and a roll; the panel's quad uses the head's
    // yaw/pitch (parallel to the view plane) and no roll.
    splat::CameraPose head;
    head.position[0] = 0.10f;
    head.position[1] = 1.62f;
    head.position[2] = -0.20f;
    head.yaw = 0.35f;
    head.pitch = 0.28f;
    head.roll = 0.30f;
    panel.update(head.yaw, head.pitch, 0.016);
    const splat::OverlayQuad quad = panel.quad(head.position, head.yaw, head.pitch);

    // The 3D shape is rigid: unit, orthogonal right/up, constant width/height.
    CHECK(dot3(quad.right, quad.up) == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(quad.width == doctest::Approx(1.4f));
    CHECK(quad.height == doctest::Approx(0.7875f));
    // Upright: the top edge is level (right has no vertical component), so a
    // head roll cannot bank the panel.
    CHECK(quad.right[1] == doctest::Approx(0.0f).epsilon(1e-5));

    // Project the four corners: (+u,+v), (-u,+v), (-u,-v), (+u,-v).
    Pixel corner[4];
    for (int i = 0; i < 4; ++i) {
        const float su = (i == 0 || i == 3) ? 1.0f : -1.0f;
        const float sv = (i < 2) ? 1.0f : -1.0f;
        float point[3];
        for (int axis = 0; axis < 3; ++axis) {
            point[axis] = quad.center[axis] + 0.5f * su * quad.width * quad.right[axis] +
                          0.5f * sv * quad.height * quad.up[axis];
        }
        corner[i] = project(head, point);
    }
    const double top = distance(corner[0], corner[1]);
    const double bottom = distance(corner[3], corner[2]);
    const double left = distance(corner[1], corner[2]);
    const double right = distance(corner[0], corner[3]);
    const double diagonal_a = distance(corner[1], corner[3]);
    const double diagonal_b = distance(corner[0], corner[2]);
    MESSAGE("panel corners: top " << top << " bottom " << bottom << " left " << left << " right "
                                  << right);
    // Parallel to the image plane: no keystone, top == bottom, left == right,
    // and equal diagonals. The roll rotates the whole rectangle in the image.
    CHECK(top == doctest::Approx(bottom).epsilon(0.01));
    CHECK(left == doctest::Approx(right).epsilon(0.01));
    CHECK(diagonal_a == doctest::Approx(diagonal_b).epsilon(0.01));
    // Aspect: a 1.4 x 0.7875 m rectangle at one depth keeps its ratio.
    CHECK(top / left == doctest::Approx(1.4 / 0.7875).epsilon(0.02));
    // The projected centre is the average of the corners.
    const Pixel centre{(corner[0].x + corner[1].x + corner[2].x + corner[3].x) * 0.25,
                       (corner[0].y + corner[1].y + corner[2].y + corner[3].y) * 0.25};
    const Pixel expected = project(head, quad.center);
    CHECK(centre.x == doctest::Approx(expected.x).epsilon(1e-6));
    CHECK(centre.y == doctest::Approx(expected.y).epsilon(1e-6));
}

TEST_CASE("vr8 comfort and display settings round-trip through ui.json") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "splat-vr8-ui-state-test";
    std::error_code error;
    std::filesystem::remove_all(dir, error);

    splat::UiState state;
    state.scale_around = 1;
    state.move_speed = 4.0f;
    state.turn_speed = 90.0f;
    state.edge_softness = 0.5f;
    state.blend_linear = false;
    REQUIRE(splat::save_ui_state(splat::Path(dir.string()), state).ok());

    const splat::UiState loaded = splat::load_ui_state(splat::Path(dir.string()));
    CHECK(loaded.scale_around == 1);
    CHECK(loaded.move_speed == doctest::Approx(4.0f));
    CHECK(loaded.turn_speed == doctest::Approx(90.0f));
    CHECK(loaded.edge_softness == doctest::Approx(0.5f));
    CHECK_FALSE(loaded.blend_linear);
    std::filesystem::remove_all(dir, error);
}
