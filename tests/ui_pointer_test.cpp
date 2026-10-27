// The Sense laser pointer's maths (core/ui/pointer.h): the ray/panel hit test
// the right controller drives, the tracker -> scene ray that carries the
// controller pose into the panel's frame, and the deterministic smoothing.
//
// This is the host test for W25. The real tracker read (hmd2 controller_pose)
// and the Sense registration are console-only and marked UNVERIFIED on
// hardware; the runbook's morning check (docs/console-runbook.md section 1f)
// covers them.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>

#include "render/camera.h"
#include "render/overlay_quad.h"
#include "ui/pointer.h"

namespace {

constexpr float kTolerance = 1e-3f;

// A 2 m x 1 m panel 2 m ahead, upright in the scene's OpenCV axes: +x right,
// up is -y, the texture's top edge is the up direction.
splat::OverlayQuad upright_panel() {
    splat::OverlayQuad panel;
    panel.center[0] = 0.0f;
    panel.center[1] = 0.0f;
    panel.center[2] = 2.0f;
    panel.right[0] = 1.0f;
    panel.right[1] = 0.0f;
    panel.right[2] = 0.0f;
    panel.up[0] = 0.0f;
    panel.up[1] = -1.0f;
    panel.up[2] = 0.0f;
    panel.width = 2.0f;
    panel.height = 1.0f;
    return panel;
}

void expect_hit(const std::optional<splat::PanelHit>& hit, float x, float y) {
    REQUIRE(hit.has_value());
    CHECK(hit->x == doctest::Approx(x).epsilon(kTolerance));
    CHECK(hit->y == doctest::Approx(y).epsilon(kTolerance));
}

}  // namespace

TEST_CASE("ui_pointer ray hits the panel centre") {
    const splat::OverlayQuad panel = upright_panel();
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float direction[3] = {0.0f, 0.0f, 1.0f};
    expect_hit(splat::ray_panel_hit(origin, direction, panel, 1280, 720), 640.0f, 360.0f);
}

TEST_CASE("ui_pointer ray hits the four panel corners") {
    const splat::OverlayQuad panel = upright_panel();
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    struct Corner {
        float x, y, z;
        float px, py;
    };
    const Corner corners[4] = {
        {-1.0f, -0.5f, 2.0f, 0.0f, 0.0f},     // top left
        {1.0f, -0.5f, 2.0f, 1280.0f, 0.0f},   // top right
        {-1.0f, 0.5f, 2.0f, 0.0f, 720.0f},    // bottom left
        {1.0f, 0.5f, 2.0f, 1280.0f, 720.0f},  // bottom right
    };
    for (const Corner& corner : corners) {
        const float direction[3] = {corner.x, corner.y, corner.z};
        expect_hit(splat::ray_panel_hit(origin, direction, panel, 1280, 720), corner.px,
                   corner.py);
    }
}

TEST_CASE("ui_pointer misses a panel behind the ray and a parallel ray") {
    const splat::OverlayQuad panel = upright_panel();
    const float behind_origin[3] = {0.0f, 0.0f, 3.0f};
    const float forward[3] = {0.0f, 0.0f, 1.0f};
    CHECK_FALSE(
        splat::ray_panel_hit(behind_origin, forward, panel, 1280, 720).has_value());

    const float away_origin[3] = {0.0f, 0.0f, 0.0f};
    const float backward[3] = {0.0f, 0.0f, -1.0f};
    CHECK_FALSE(splat::ray_panel_hit(away_origin, backward, panel, 1280, 720).has_value());

    const float parallel[3] = {1.0f, 0.0f, 0.0f};
    CHECK_FALSE(splat::ray_panel_hit(away_origin, parallel, panel, 1280, 720).has_value());
}

TEST_CASE("ui_pointer misses outside the panel rectangle") {
    const splat::OverlayQuad panel = upright_panel();
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    // On the panel's plane (z = 2) but 1.5 m to the right: past the 1 m half
    // width.
    const float direction[3] = {1.5f, 0.0f, 2.0f};
    CHECK_FALSE(splat::ray_panel_hit(origin, direction, panel, 1280, 720).has_value());

    // Above the top edge (0.75 m up, the half height is 0.5 m).
    const float above[3] = {0.0f, -0.75f, 2.0f};
    CHECK_FALSE(splat::ray_panel_hit(origin, above, panel, 1280, 720).has_value());
}

TEST_CASE("ui_pointer maps a tilted panel to canvas pixels") {
    splat::OverlayQuad panel = upright_panel();
    const float angle = 0.78539816339f;  // 45 degrees about scene y
    panel.right[0] = std::cos(angle);
    panel.right[1] = 0.0f;
    panel.right[2] = -std::sin(angle);
    // A point half way along right (+u) and a quarter up (+v): 0.75 of the
    // width and 0.25 of the height.
    const float target[3] = {
        panel.center[0] + panel.right[0] * 0.5f,
        panel.center[1] - 0.25f,
        panel.center[2] + panel.right[2] * 0.5f,
    };
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    expect_hit(splat::ray_panel_hit(origin, target, panel, 1280, 720), 960.0f, 180.0f);
}

TEST_CASE("ui_pointer rejects a zero-sized canvas and an empty panel") {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const float direction[3] = {0.0f, 0.0f, 1.0f};
    CHECK_FALSE(splat::ray_panel_hit(origin, direction, upright_panel(), 0, 720).has_value());
    CHECK_FALSE(splat::ray_panel_hit(origin, direction, upright_panel(), 1280, 0).has_value());
    splat::OverlayQuad empty = upright_panel();
    empty.width = 0.0f;
    CHECK_FALSE(splat::ray_panel_hit(origin, direction, empty, 1280, 720).has_value());
}

TEST_CASE("ui_pointer controller ray is the head-relative pointer in the scene") {
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float head[3] = {0.0f, 1.6f, 0.0f};
    // 20 cm above and 50 cm in front of the head in tracker axes (+y up, -z
    // forward): 20 cm down and 50 cm forward in scene axes.
    const float controller[3] = {0.0f, 1.8f, -0.5f};
    splat::CameraPose head_scene;
    head_scene.position[0] = 0.0f;
    head_scene.position[1] = 1.6f;
    head_scene.position[2] = 0.0f;

    const splat::PointerRay ray =
        splat::controller_ray(head, identity, controller, identity, head_scene);
    CHECK(ray.origin[0] == doctest::Approx(0.0f).epsilon(kTolerance));
    CHECK(ray.origin[1] == doctest::Approx(1.4f).epsilon(kTolerance));
    CHECK(ray.origin[2] == doctest::Approx(0.5f).epsilon(kTolerance));
    CHECK(ray.direction[0] == doctest::Approx(0.0f).epsilon(kTolerance));
    CHECK(ray.direction[1] == doctest::Approx(0.0f).epsilon(kTolerance));
    CHECK(ray.direction[2] == doctest::Approx(1.0f).epsilon(kTolerance));
}

TEST_CASE("ui_pointer controller ray follows the controller's turn") {
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // 90 degrees about tracker y turns the controller's -z forward to -x.
    const float half = 0.70710678118f;
    const float turned[4] = {0.0f, half, 0.0f, half};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const float controller[3] = {0.0f, 0.0f, -1.0f};
    const splat::CameraPose head_scene;

    const splat::PointerRay ray =
        splat::controller_ray(zero, identity, controller, turned, head_scene);
    CHECK(ray.origin[2] == doctest::Approx(1.0f).epsilon(kTolerance));
    CHECK(ray.direction[0] == doctest::Approx(-1.0f).epsilon(kTolerance));
    CHECK(ray.direction[1] == doctest::Approx(0.0f).epsilon(kTolerance));
    CHECK(ray.direction[2] == doctest::Approx(0.0f).epsilon(kTolerance));
}

TEST_CASE("ui_pointer controller ray turns with the head's scene yaw") {
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const float controller[3] = {0.0f, 0.0f, -1.0f};
    splat::CameraPose head_scene;
    head_scene.yaw = 1.57079632679f;  // 90 degrees right: scene forward is +x

    const splat::PointerRay ray =
        splat::controller_ray(zero, identity, controller, identity, head_scene);
    CHECK(ray.origin[0] == doctest::Approx(1.0f).epsilon(kTolerance));
    CHECK(ray.origin[2] == doctest::Approx(0.0f).epsilon(kTolerance));
    CHECK(ray.direction[0] == doctest::Approx(1.0f).epsilon(kTolerance));
    CHECK(ray.direction[2] == doctest::Approx(0.0f).epsilon(kTolerance));
}

TEST_CASE("ui_pointer filter converges and resets") {
    splat::PointerFilter filter(0.1f);
    const double dt = 1.0 / 60.0;

    const std::optional<splat::PanelHit> first =
        filter.update(splat::PanelHit{100.0f, 100.0f}, dt);
    REQUIRE(first.has_value());
    CHECK(first->x == doctest::Approx(100.0f));
    CHECK(first->y == doctest::Approx(100.0f));

    float previous = 100.0f;
    for (int frame = 0; frame < 120; ++frame) {
        const std::optional<splat::PanelHit> step =
            filter.update(splat::PanelHit{200.0f, 100.0f}, dt);
        REQUIRE(step.has_value());
        CHECK(step->x >= previous);  // monotonic, no overshoot
        CHECK(step->x <= 200.0f);
        previous = step->x;
    }
    CHECK(filter.valid());
    CHECK(filter.value().x == doctest::Approx(200.0f).epsilon(kTolerance));
    CHECK(filter.value().y == doctest::Approx(100.0f).epsilon(kTolerance));

    // A frame with no hit clears the pointer: the next hit starts fresh.
    CHECK_FALSE(filter.update(std::nullopt, dt).has_value());
    CHECK_FALSE(filter.valid());
    const std::optional<splat::PanelHit> again =
        filter.update(splat::PanelHit{10.0f, 20.0f}, dt);
    REQUIRE(again.has_value());
    CHECK(again->x == doctest::Approx(10.0f));
    CHECK(again->y == doctest::Approx(20.0f));
}
