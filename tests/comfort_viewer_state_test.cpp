#include <doctest/doctest.h>

#include <cmath>

#include "render/viewer_state.h"

namespace {

constexpr float kEpsilon = 1.0e-5f;

bool near(float a, float b, float epsilon = kEpsilon) {
    return std::abs(a - b) <= epsilon;
}

}  // namespace

TEST_CASE("comfort fade follows head distance from the capture point") {
    splat::ViewerState state;
    CHECK(state.comfort_radius() == doctest::Approx(splat::kDefaultComfortRadius));
    // The fade now starts at half the radius, not 80 %: 12.5 cm of travel at
    // the default radius instead of 5 cm.
    CHECK(splat::kDefaultFadeStart == doctest::Approx(0.5f));
    CHECK(state.fade_start() == doctest::Approx(0.5f));

    // 1 inside 50 % of the radius, smoothstep down to 0 at the radius.
    CHECK(state.fade_for_distance(0.0f) == 1.0f);
    CHECK(state.fade_for_distance(0.3f * state.comfort_radius()) == 1.0f);
    CHECK(state.fade_for_distance(0.5f * state.comfort_radius()) == 1.0f);
    // t = 0.5 halfway through the fade: 1 - smoothstep(0.5) = 0.5.
    CHECK(state.fade_for_distance(0.75f * state.comfort_radius()) == doctest::Approx(0.5f));
    // 60 % is t = 0.2, 1 - smoothstep(0.2) = 0.896; 90 % is t = 0.8, 0.104.
    CHECK(state.fade_for_distance(0.6f * state.comfort_radius()) == doctest::Approx(0.896f));
    CHECK(state.fade_for_distance(0.9f * state.comfort_radius()) == doctest::Approx(0.104f));
    CHECK(state.fade_for_distance(state.comfort_radius()) == 0.0f);
    CHECK(state.fade_for_distance(1.2f * state.comfort_radius()) == 0.0f);

    // Distance only: the curve has no time term, so repeated calls and any
    // frame rate give the same fade.
    CHECK(state.fade_for_distance(0.6f * state.comfort_radius()) ==
          state.fade_for_distance(0.6f * state.comfort_radius()));

    // Monotone and bounded across the whole span.
    float previous = 1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float distance = 1.2f * state.comfort_radius() * static_cast<float>(i) / 100.0f;
        const float fade = state.fade_for_distance(distance);
        CHECK(fade <= previous + kEpsilon);
        CHECK(fade >= -kEpsilon);
        CHECK(fade <= 1.0f + kEpsilon);
        previous = fade;
    }
}

TEST_CASE("comfort fade is measured from the anchor") {
    splat::ViewerState state;
    const float head[3] = {0.10f, 0.0f, 0.0f};  // 40 %, inside the fade start
    CHECK(state.comfort_fade(head) == 1.0f);
    const float far_head[3] = {0.30f, 0.0f, 0.0f};
    CHECK(state.comfort_fade(far_head) == 0.0f);

    state.recentre(far_head, 0.0f);
    // The anchor moved with the head, so the same position is now the capture
    // point and the fade resets.
    CHECK(state.comfort_fade(far_head) == 1.0f);
    CHECK(state.head_distance(far_head) == doctest::Approx(0.0f));
}

TEST_CASE("comfort fade start and radius are adjustable and clamped") {
    splat::ViewerState state;
    state.set_comfort_radius(0.5f);
    CHECK(state.comfort_radius() == doctest::Approx(0.5f));
    state.set_fade_start(0.5f);
    CHECK(state.fade_for_distance(0.25f) == 1.0f);
    CHECK(state.fade_for_distance(0.375f) == doctest::Approx(0.5f));
    CHECK(state.fade_for_distance(0.5f) == 0.0f);

    state.set_comfort_radius(0.001f);
    CHECK(state.comfort_radius() == doctest::Approx(splat::kMinComfortRadius));
    state.set_comfort_radius(100.0f);
    CHECK(state.comfort_radius() == doctest::Approx(splat::kMaxComfortRadius));
    state.set_fade_start(-1.0f);
    CHECK(state.fade_start() == 0.0f);
    state.set_fade_start(2.0f);
    CHECK(state.fade_start() == 1.0f);
}

TEST_CASE("recentre re-anchors at the head and zeroes yaw only") {
    splat::ViewerState state;
    const float head[3] = {1.0f, 0.5f, 2.0f};
    const float yaw = 0.7f;
    state.recentre(head, yaw);

    CHECK(state.anchor()[0] == doctest::Approx(head[0]));
    CHECK(state.anchor()[1] == doctest::Approx(head[1]));
    CHECK(state.anchor()[2] == doctest::Approx(head[2]));

    const splat::CameraPose pose = state.scene_pose(head, yaw, 0.3f);
    CHECK(pose.position[0] == doctest::Approx(0.0f));
    CHECK(pose.position[1] == doctest::Approx(0.0f));
    CHECK(pose.position[2] == doctest::Approx(0.0f));
    CHECK(pose.yaw == doctest::Approx(0.0f));
    // Recentre is yaw only: pitch survives.
    CHECK(pose.pitch == doctest::Approx(0.3f));

    // Moving one metre along the recentred heading maps to scene +z.
    const float forward[3] = {head[0] + std::sin(yaw), head[1], head[2] + std::cos(yaw)};
    const splat::CameraPose moved = state.scene_pose(forward, yaw, 0.0f);
    CHECK(moved.position[0] == doctest::Approx(0.0f).epsilon(1.0e-4));
    CHECK(moved.position[1] == doctest::Approx(0.0f));
    CHECK(moved.position[2] == doctest::Approx(1.0f).epsilon(1.0e-4));
    CHECK(moved.yaw == doctest::Approx(0.0f));

    // A 30 degree head turn after the recentre is a 30 degree scene yaw.
    const splat::CameraPose turned = state.scene_pose(head, yaw + 0.5f, 0.0f);
    CHECK(turned.yaw == doctest::Approx(0.5f));

    // Strafing right after the recentre maps to scene +x.
    const float right[3] = {head[0] + std::cos(yaw), head[1], head[2] - std::sin(yaw)};
    const splat::CameraPose strafed = state.scene_pose(right, yaw, 0.0f);
    CHECK(strafed.position[0] == doctest::Approx(1.0f).epsilon(1.0e-4));
    CHECK(strafed.position[2] == doctest::Approx(0.0f).epsilon(1.0e-4));
}

TEST_CASE("world scale is clamped and does not move the camera") {
    splat::ViewerState state;
    const float head[3] = {0.1f, 0.0f, -0.2f};
    const splat::CameraPose before = state.scene_pose(head, 0.2f, 0.1f);

    state.set_world_scale(0.01f);
    CHECK(state.world_scale() == doctest::Approx(splat::kMinWorldScale));
    state.set_world_scale(100.0f);
    CHECK(state.world_scale() == doctest::Approx(splat::kMaxWorldScale));
    state.set_world_scale(1.0f);
    state.scale_world(2.0f);
    CHECK(state.world_scale() == doctest::Approx(2.0f));

    const splat::CameraPose after = state.scene_pose(head, 0.2f, 0.1f);
    for (int i = 0; i < 3; ++i) CHECK(after.position[i] == doctest::Approx(before.position[i]));

    const splat::FrameView view = state.frame(head, 0.2f, 0.1f);
    CHECK(view.scene_scale == doctest::Approx(2.0f));
    CHECK(view.comfort_fade == doctest::Approx(state.comfort_fade(head)));
}

TEST_CASE("viewer state pause flag is carried for auto-pause") {
    splat::ViewerState state;
    CHECK_FALSE(state.paused());
    state.set_paused(true);
    CHECK(state.paused());
    state.set_paused(false);
    CHECK_FALSE(state.paused());
}
