// VR9: maquette scaling (the scene lands on a virtual table, then scales
// there), the keyboard's key repeat and the overlay mip chain's level maths.
// The geometry is CPU-only: the same HmdView and free functions the console
// runs, driven with a synthetic head and splat cloud.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "render/hmd_view.h"
#include "ui/app.h"
#include "ui/ui_input.h"
#include "ui/ui_state.h"

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

// `near_count` points at `near_z` and the rest at `far_z`, all inside the
// 15 deg cone around +z.
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

// The same tracked pose turned `degrees` about the tracker's up axis (a yaw;
// the scene yaw comes out at the negative of it, see hmd_forward()).
splat::HeadPoses yawed_head(double degrees) {
    splat::HeadPoses poses = identity_head();
    const double half = 0.5 * degrees * kPi / 180.0;
    for (splat::TrackedPose* pose : {&poses.head, &poses.left, &poses.right}) {
        pose->quaternion[1] = static_cast<float>(std::sin(half));
        pose->quaternion[3] = static_cast<float>(std::cos(half));
    }
    return poses;
}

void check3(const float actual[3], float x, float y, float z, float epsilon = 1.0e-5f) {
    CHECK(actual[0] == doctest::Approx(x).epsilon(epsilon));
    CHECK(actual[1] == doctest::Approx(y).epsilon(epsilon));
    CHECK(actual[2] == doctest::Approx(z).epsilon(epsilon));
}

// Where a scene-space point is displayed: w = s*x + offset.
void displayed(const splat::HmdView& view, const float point[3], float out[3]) {
    const float* offset = view.scene_offset();
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = view.world_scale() * point[axis] + offset[axis];
    }
}

}  // namespace

TEST_CASE("vr9 maquette blend is a smoothstep from the placement to the table") {
    const float scale_min = 0.25f;
    CHECK(splat::maquette_blend(1.0f, scale_min) == doctest::Approx(0.0f));
    CHECK(splat::maquette_blend(1.5f, scale_min) == doctest::Approx(0.0f));
    CHECK(splat::maquette_blend(scale_min, scale_min) == doctest::Approx(1.0f));
    CHECK(splat::maquette_blend(0.1f, scale_min) == doctest::Approx(1.0f));
    // Halfway through the interval: t = 0.5, smoothstep(0.5) = 0.5.
    const float half = 0.5f * (1.0f + scale_min);
    CHECK(splat::maquette_blend(half, scale_min) == doctest::Approx(0.5f));
    // Monotone and continuous at both ends.
    float previous = 0.0f;
    for (int step = 0; step <= 100; ++step) {
        const float scale = 1.0f - 0.75f * static_cast<float>(step) / 100.0f;
        const float t = splat::maquette_blend(scale, scale_min);
        CHECK(t >= previous - 1.0e-6f);
        previous = t;
    }
    // No glide when the scene already fits the table.
    CHECK(splat::maquette_blend(1.0f, 1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("vr9 maquette centre glides from C0 to T and scales about T above 1") {
    const float c0[3] = {0.0f, 0.0f, 2.0f};
    const float table[3] = {0.0f, 0.35f, 0.7f};
    const float scale_min = 0.25f;
    float out[3];

    splat::maquette_centre(1.0f, c0, table, scale_min, out);
    check3(out, 0.0f, 0.0f, 2.0f);
    // Continuity at s_m: both the glide (t = 1) and the table branch give T.
    splat::maquette_centre(scale_min, c0, table, scale_min, out);
    check3(out, 0.0f, 0.35f, 0.7f);
    splat::maquette_centre(0.2f, c0, table, scale_min, out);
    check3(out, 0.0f, 0.35f, 0.7f);
    // Halfway: the 3D centre is halfway between C0 and T.
    const float half = 0.5f * (1.0f + scale_min);
    splat::maquette_centre(half, c0, table, scale_min, out);
    check3(out, 0.0f, 0.175f, 1.35f);
    // Above 1: pure scaling about T, continuous at C(1) = C0.
    splat::maquette_centre(2.0f, c0, table, scale_min, out);
    check3(out, 0.0f, -0.35f, 0.7f + 2.0f * (2.0f - 0.7f));
    splat::maquette_centre(1.0f, c0, table, scale_min, out);
    check3(out, 0.0f, 0.0f, 2.0f);
}

TEST_CASE("vr9 s_m is the scale at which the robust box is 0.6 m wide") {
    // E * s_m = 0.6, within the world-scale range and never above 1.
    CHECK(splat::maquette_scale_for_extent(1.2f) == doctest::Approx(0.5f));
    CHECK(splat::maquette_scale_for_extent(2.4f) == doctest::Approx(0.25f));
    CHECK(splat::maquette_scale_for_extent(4.0f) ==
          doctest::Approx(splat::kMinWorldScale));  // clamped
    CHECK(splat::maquette_scale_for_extent(0.3f) == doctest::Approx(1.0f));
    CHECK(splat::maquette_scale_for_extent(0.0f) == doctest::Approx(1.0f));

    // The robust extent is the 5-95 percentile box's larger horizontal span.
    std::vector<float> cloud;
    for (int i = 0; i <= 100; ++i) {
        cloud.push_back(-2.0f + 0.04f * static_cast<float>(i));
        cloud.push_back(0.0f);
        cloud.push_back(1.0f);
    }
    CHECK(splat::robust_scene_extent(cloud.data(), cloud.size() / 3, 1.0f) ==
          doctest::Approx(3.6f).epsilon(0.01));
    // The extent scales with the scene's scale, so s_m is scale independent.
    CHECK(splat::robust_scene_extent(cloud.data(), cloud.size() / 3, 0.5f) ==
          doctest::Approx(1.8f).epsilon(0.01));
    CHECK(splat::robust_scene_extent(nullptr, 0, 1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("vr9 a maquette press lands the content on T and comes back exactly") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::maquette);
    // z span 0.8 -> 4.0 = 3.2 m; s_m = 0.6 / 3.2 = 0.1875, clamped to 0.25.
    view.set_pivot_samples(two_depth_cloud(0.8f, 100, 4.0f, 900));
    view.begin_scale_gesture();

    CHECK(view.maquette_scale() == doctest::Approx(0.25f));
    CHECK(view.maquette_extent() == doctest::Approx(3.2f).epsilon(0.02));
    // T: 0.7 m ahead, 0.35 m below the eyes; the marker is drawn there.
    CHECK(view.maquette_table()[1] == doctest::Approx(0.35f));
    CHECK(view.maquette_table()[2] == doctest::Approx(0.7f));
    check3(view.scale_pivot_world(), 0.0f, 0.35f, 0.7f);
    // C0 is the visible cone's median (the far group), not the eye.
    CHECK(view.maquette_content_centre()[2] == doctest::Approx(4.0f).epsilon(0.02));

    // Shrink in many small steps: the content centre glides and ends on T.
    for (int step = 0; step < 300; ++step) view.scale_scene(0.995f);
    CHECK(view.world_scale() == doctest::Approx(0.25f).epsilon(1e-3));
    float centre[3];
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, 0.0f, 0.35f, 0.7f, 1.0e-3f);
    // Exactly scaling about T below s_m: offset = T - s * anchor.
    const float* anchor = view.maquette_anchor();
    const float* offset = view.scene_offset();
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(offset[axis] ==
              doctest::Approx(view.maquette_table()[axis] - view.world_scale() * anchor[axis])
                  .epsilon(1e-4));
    }

    // The round trip: growing back to 1 returns the original centre and offset.
    const float c0[3] = {view.maquette_content_centre()[0], view.maquette_content_centre()[1],
                         view.maquette_content_centre()[2]};
    view.scale_scene(1.0f / view.world_scale());
    CHECK(view.world_scale() == doctest::Approx(1.0f).epsilon(1e-5));
    displayed(view, anchor, centre);
    check3(centre, c0[0], c0[1], c0[2], 1.0e-3f);
    CHECK(view.scene_offset()[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(view.scene_offset()[1] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(view.scene_offset()[2] == doctest::Approx(0.0f).epsilon(1e-4));

    // Above 1 the scene grows about T; still bigger on screen than at 1.
    view.scale_scene(2.0f);
    CHECK(view.world_scale() == doctest::Approx(2.0f));
    displayed(view, anchor, centre);
    for (int axis = 0; axis < 3; ++axis) {
        CHECK(centre[axis] == doctest::Approx(view.maquette_table()[axis] +
                                              2.0f * (c0[axis] - view.maquette_table()[axis]))
                                  .epsilon(1e-4));
    }

    // Recentre puts back scale 1 and the original placement.
    view.reset_scene_adjust();
    CHECK(view.world_scale() == doctest::Approx(1.0f));
    CHECK(view.scene_offset()[2] == doctest::Approx(0.0f));
}

TEST_CASE("vr9 the maquette grows and shrinks in place once it is on the table") {
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::maquette);
    // z span 1.2 m -> s_m = 0.5, so a press can go below it.
    view.set_pivot_samples(two_depth_cloud(0.8f, 100, 2.0f, 900));
    view.begin_scale_gesture();
    CHECK(view.maquette_scale() == doctest::Approx(0.5f));
    const float c0[3] = {view.maquette_content_centre()[0], view.maquette_content_centre()[1],
                         view.maquette_content_centre()[2]};
    // At s_m the scene has landed; halfway through the glide the centre is
    // halfway (smoothstep 0.5).
    view.scale_scene(0.75f);
    float centre[3];
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, 0.5f * c0[0], 0.175f + 0.5f * c0[1], 1.35f, 1.0e-3f);
    view.scale_scene(0.5f / view.world_scale());
    CHECK(view.world_scale() == doctest::Approx(0.5f));
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, 0.0f, 0.35f, 0.7f, 1.0e-3f);
    // Below s_m: still on T, only the scale changes (a maquette on the table).
    view.scale_scene(0.6f);
    CHECK(view.world_scale() == doctest::Approx(0.3f));
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, 0.0f, 0.35f, 0.7f, 1.0e-4f);
    // And back out: table first, then the glide home.
    view.scale_scene(1.0f / 0.3f);
    CHECK(view.world_scale() == doctest::Approx(1.0f).epsilon(1e-5));
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, c0[0], c0[1], c0[2], 1.0e-3f);
}

TEST_CASE("vr9 separate maquette presses glide back to the original placement") {
    // Review fix: T, C0 and s_m are captured once, when the shrink starts from
    // 1.0, and persist across presses until the scale comes back to 1.0 (or a
    // recentre / scene change). Re-deriving them per press made a second press
    // at 0.5x compute s_m from the displayed extent and C0 from the table, so
    // growing back could not reach the original placement.
    splat::HmdView view;
    REQUIRE(view.place(identity_head()));
    view.set_scale_pivot_mode(splat::ScalePivotMode::maquette);
    // z span 1.2 m -> s_m = 0.5, above the 0.25 world-scale floor, so a second
    // press can go below the landing scale and back.
    view.set_pivot_samples(two_depth_cloud(0.8f, 100, 2.0f, 900));

    view.begin_scale_gesture();
    const float table[3] = {view.maquette_table()[0], view.maquette_table()[1],
                            view.maquette_table()[2]};
    const float c0[3] = {view.maquette_content_centre()[0], view.maquette_content_centre()[1],
                         view.maquette_content_centre()[2]};
    const float anchor[3] = {view.maquette_anchor()[0], view.maquette_anchor()[1],
                             view.maquette_anchor()[2]};
    const float s_m = view.maquette_scale();
    CHECK(s_m == doctest::Approx(0.5f));
    CHECK(view.maquette_extent() == doctest::Approx(1.2f).epsilon(0.02));

    // Press 1: down to ~0.74, release.
    for (int step = 0; step < 60; ++step) view.scale_scene(0.995f);
    view.end_scale_gesture();
    CHECK(view.world_scale() < 0.8f);
    CHECK_FALSE(view.scale_gesture());

    // The user turns their head 20 deg before the next press: the table point
    // must stay where the scene landed, not jump in front of the new gaze.
    REQUIRE(view.update(yawed_head(20.0)));
    view.begin_scale_gesture();
    check3(view.maquette_table(), table[0], table[1], table[2], 1.0e-6f);
    check3(view.maquette_content_centre(), c0[0], c0[1], c0[2], 1.0e-6f);
    check3(view.maquette_anchor(), anchor[0], anchor[1], anchor[2], 1.0e-6f);
    CHECK(view.maquette_scale() == doctest::Approx(s_m));
    CHECK(view.maquette_extent() == doctest::Approx(1.2f).epsilon(0.02));

    // Press 2: down past the landing scale, release on the table.
    for (int step = 0; step < 120; ++step) view.scale_scene(0.99f);
    view.end_scale_gesture();
    REQUIRE(view.world_scale() < s_m);
    float centre[3];
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, table[0], table[1], table[2], 1.0e-3f);

    // Press 3: up to exactly 1.0: the original placement, not an offset left
    // over from the second press's re-capture.
    view.begin_scale_gesture();
    view.scale_scene(1.0f / view.world_scale());
    CHECK(view.world_scale() == doctest::Approx(1.0f).epsilon(1.0e-5));
    check3(view.scene_offset(), 0.0f, 0.0f, 0.0f, 1.0e-3f);
    displayed(view, view.maquette_anchor(), centre);
    check3(centre, c0[0], c0[1], c0[2], 1.0e-3f);
    view.end_scale_gesture();

    // The round trip ended at 1.0: the next shrink captures a fresh table
    // point, in front of where the head looks now (20 deg off the first).
    view.begin_scale_gesture();
    const double yaw = view.view_yaw();
    CHECK(view.maquette_table()[0] ==
          doctest::Approx(0.7 * std::sin(yaw)).epsilon(1.0e-4));
    CHECK(view.maquette_table()[2] ==
          doctest::Approx(0.7 * std::cos(yaw)).epsilon(1.0e-4));
    CHECK(std::fabs(view.maquette_table()[0] - table[0]) > 0.1f);

    // A recentre ends the round trip too: scale 1 and a fresh capture.
    view.scale_scene(0.5f);
    view.end_scale_gesture();
    view.reset_scene_adjust();
    CHECK(view.world_scale() == doctest::Approx(1.0f));
    view.begin_scale_gesture();
    CHECK(view.maquette_scale() == doctest::Approx(s_m));
    CHECK(view.maquette_extent() == doctest::Approx(1.2f).epsilon(0.02));
}

TEST_CASE("vr9 key repeat waits 350 ms then accelerates from 8 to 20 steps/s") {
    // The rate curve: 8 steps/s just after the delay, 20 by the end of the
    // 1.2 s ramp and after it.
    CHECK(splat::UiRepeat::interval_for_held(splat::kRepeatDelaySeconds) ==
          doctest::Approx(1.0 / splat::kRepeatStartHz));
    CHECK(splat::UiRepeat::interval_for_held(splat::kRepeatDelaySeconds +
                                             splat::kRepeatRampSeconds) ==
          doctest::Approx(1.0 / splat::kRepeatEndHz));
    CHECK(splat::UiRepeat::interval_for_held(30.0) ==
          doctest::Approx(1.0 / splat::kRepeatEndHz));

    splat::UiRepeat repeat;
    // The press frame is the caller's own step; the repeat does not repeat it.
    CHECK(repeat.update(splat::Down, splat::Down, 0.0) == 0);
    double elapsed = 0.0;
    int repeats = 0;
    double first_repeat = -1.0;
    const double dt = 1.0 / 120.0;
    for (int frame = 0; frame < 240; ++frame) {  // 2 s
        elapsed += dt;
        if (repeat.update(0, splat::Down, dt) != 0) {
            ++repeats;
            if (first_repeat < 0.0) first_repeat = elapsed;
        }
    }
    // No repeat in the first 340 ms; the first arrives at ~350 ms.
    CHECK(first_repeat >= splat::kRepeatDelaySeconds - dt);
    CHECK(first_repeat <= splat::kRepeatDelaySeconds + 2.0 * dt);
    // A second later the rate is well above the starting 8/s: about 12-20.
    CHECK(repeats >= 20);
    CHECK(repeats <= 34);
    // Released: no more repeats and the next press starts the delay again.
    CHECK(repeat.update(0, 0, dt) == 0);
    CHECK(repeat.update(splat::Up, splat::Up, dt) == 0);
    CHECK(repeat.update(0, splat::Up, 0.34) == 0);
    CHECK(repeat.update(0, splat::Up, 0.02) != 0);
}

TEST_CASE("vr9 a held direction reaches every screen through UiApp") {
    // UiApp folds the repeat edges into each frame's `pressed`, so a screen
    // sees ordinary presses and every list/grid/menu gets the acceleration.
    class Counter : public splat::Screen {
    public:
        void update(splat::UiContext&, const splat::UiInput& input) override {
            if ((input.pressed & splat::Down) != 0) ++presses;
        }
        void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
        std::string title() const override { return "counter"; }
        int presses = 0;
    };
    splat::UiServices services;
    splat::UiApp app(services);
    Counter* counter = nullptr;
    app.registry().add(splat::ScreenId::Albums, [&counter] {
        auto screen = std::make_unique<Counter>();
        counter = screen.get();
        return screen;
    });
    app.start();

    splat::UiInput press;
    press.pressed = splat::Down;
    press.held = splat::Down;
    press.dt = 1.0 / 60.0;
    app.update(press);
    REQUIRE(counter != nullptr);
    CHECK(counter->presses == 1);
    splat::UiInput hold;
    hold.held = splat::Down;
    hold.dt = 1.0 / 60.0;
    for (int frame = 0; frame < 60; ++frame) app.update(hold);
    // One second of holding after the press: the delay plus ~8-20/s repeats.
    CHECK(counter->presses >= 6);
    CHECK(counter->presses <= 20);
    // Release clears the repeat; an idle frame is not a press.
    app.update(splat::UiInput{});
    const int after_release = counter->presses;
    for (int frame = 0; frame < 30; ++frame) app.update(splat::UiInput{});
    CHECK(counter->presses == after_release);
}

TEST_CASE("vr9 the scale mode is Maquette by default and persists") {
    splat::UiState defaults;
    CHECK(defaults.scale_around == 2);
    CHECK(std::string(splat::scale_mode_name(defaults.scale_around)) == "Maquette");
    CHECK(std::string(splat::scale_mode_name(0)) == "Front");
    CHECK(std::string(splat::scale_mode_name(1)) == "Centre");

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "splat-vr9-ui-state-test";
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    REQUIRE(splat::save_ui_state(splat::Path(dir.string()), defaults).ok());
    CHECK(splat::load_ui_state(splat::Path(dir.string())).scale_around == 2);

    defaults.scale_around = 0;
    REQUIRE(splat::save_ui_state(splat::Path(dir.string()), defaults).ok());
    CHECK(splat::load_ui_state(splat::Path(dir.string())).scale_around == 0);
    std::filesystem::remove_all(dir, error);
}
