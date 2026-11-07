// W16: the VR session's panel state machine (platform/ps5/viewer/ui_mode.h).
//
// The panel itself is UiApp over UiHost, the same construction the TV path uses
// (platform/ps5/viewer/ui_setup.h). What this test drives is the routing around
// it, on the desktop, with button words instead of pads:
//
//   (a) Options opens the panel; inside it Options is the panel's own (it opens
//       Settings) and Circle at the panel's root emits ResumeScene, which
//       closes it; the press that opens it is never also a panel press;
//   (b) a frame goes to exactly one of the two - the panel or the scene;
//   (c) the scene's controls (mode, scale, L1/R1) are suspended while the panel
//       is up, including on the frame it opens;
//   (d) UI=0 removes the panel: every frame is the pre-W16 scene frame;
//   (e) a panel that starts open (UI=1) ignores a button held from the launch
//       screen, and a left stick moves its focus, not the viewer.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include "ui/app.h"
#include "ui/screens/registry.h"
#include "../platform/ps5/viewer/ui_mode.h"

namespace {

using viewer::PanelMode;
using viewer::pad::kCross;
using viewer::pad::kL1;
using viewer::pad::kOptions;
using viewer::pad::kR1;
using viewer::pad::kR2;
using viewer::pad::kTriangle;

// The panel is open and Options has just gone down: the one frame that opens it.
viewer::UiRoute open_panel(PanelMode& panel) {
    REQUIRE(panel.route(0).scene_controls);  // a seeded first frame
    const viewer::UiRoute route = panel.route(kOptions);
    REQUIRE(route.opened);
    REQUIRE(route.panel_visible);
    return route;
}

}  // namespace

TEST_CASE("vr_ui_state: Options opens the panel and the panel then owns Options") {
    PanelMode panel(true);
    const viewer::UiRoute opened = open_panel(panel);

    // The opening press belongs to neither side: it is not a panel press that
    // would open Settings on the same frame.
    CHECK(opened.panel_visible);
    CHECK_FALSE(opened.scene_controls);
    CHECK(opened.ui_pressed == 0u);
    CHECK(opened.ui_held == 0u);
    CHECK(panel.open());

    // Still held: no repeat, and the scene still gets nothing.
    const viewer::UiRoute held = panel.route(kOptions);
    CHECK(held.panel_visible);
    CHECK_FALSE(held.scene_controls);
    CHECK((held.ui_pressed & splat::Menu) == 0u);
    CHECK((held.ui_held & splat::Menu) != 0u);

    // Released, then pressed again: this is the panel's Options (Settings).
    CHECK(panel.route(0).panel_visible);
    const viewer::UiRoute settings = panel.route(kOptions);
    CHECK((settings.ui_pressed & splat::Menu) != 0u);
    CHECK_FALSE(settings.scene_controls);

    // Circle is the panel's Back; at its root UiApp answers with ResumeScene.
    CHECK(panel.route(0).panel_visible);
    const viewer::UiRoute circle = panel.route(viewer::pad::kCircle);
    CHECK((circle.ui_pressed & splat::Back) != 0u);
    CHECK_FALSE(circle.scene_controls);

    // The host closes it on the command; the next frame is the scene's again.
    panel.close();
    const viewer::UiRoute back = panel.route(0);
    CHECK_FALSE(back.panel_visible);
    CHECK_FALSE(back.panel_input);
    CHECK(back.scene_controls);
    CHECK(back.ui_pressed == 0u);
}

TEST_CASE("vr_ui_state: closing while Options is held does not reopen it") {
    PanelMode panel(true);
    open_panel(panel);
    // Options, released, pressed again: that press is the panel's Menu.
    CHECK(panel.route(0).panel_visible);
    CHECK((panel.route(kOptions).ui_pressed & splat::Menu) != 0u);
    // The panel closes (a scene viewed from it) with Options still down: no
    // reopen.
    panel.close();
    CHECK_FALSE(panel.route(kOptions).panel_visible);
    CHECK_FALSE(panel.route(kOptions).opened);
    // Only a fresh press opens it again.
    CHECK(panel.route(0).scene_controls);
    CHECK(panel.route(kOptions).opened);
}

TEST_CASE("vr_ui_state: a frame goes to exactly one of the panel and the scene") {
    PanelMode panel(true);
    // Closed: the scene owns the frame, the panel sees nothing.
    const viewer::UiRoute scene_frame = panel.route(kCross | kL1 | kR1);
    CHECK(scene_frame.scene_controls);
    CHECK_FALSE(scene_frame.panel_input);
    CHECK_FALSE(scene_frame.panel_visible);
    CHECK(scene_frame.ui_pressed == 0u);
    CHECK(scene_frame.ui_held == 0u);

    // Open: the panel owns it, through the same button word.
    open_panel(panel);
    panel.route(0);
    const viewer::UiRoute panel_frame = panel.route(kCross);
    CHECK(panel_frame.panel_visible);
    CHECK(panel_frame.panel_input);
    CHECK_FALSE(panel_frame.scene_controls);
    CHECK((panel_frame.ui_pressed & splat::Select) != 0u);
    CHECK((panel_frame.ui_held & splat::Select) != 0u);
}

TEST_CASE("vr_ui_state: the scene's controls are suspended while the panel is up") {
    PanelMode panel(true);
    const viewer::UiRoute opened = open_panel(panel);
    // Even the frame it opens on: a stick or a trigger held while Options goes
    // down must not keep walking or scaling the scene behind the panel.
    CHECK_FALSE(opened.scene_controls);

    // Every scene control is suspended, for as long as the panel is up and
    // whatever is held: stick deflection, L1/R1 (scene change), R2 (scale).
    const uint32_t words[] = {0u, kCross, kL1, kR1, kR2, kTriangle, kCross | kL1 | kOptions};
    for (const uint32_t word : words) {
        const viewer::UiRoute route = panel.route(word, 1.0f, -1.0f);
        CHECK_FALSE(route.scene_controls);
        CHECK(route.panel_visible);
    }

    // After ResumeScene the scene has its controls back.
    panel.close();
    CHECK(panel.route(0).scene_controls);
    CHECK(panel.route(kCross).scene_controls);
}

TEST_CASE("vr_ui_state: no panel when UI is off in viewer.env") {
    PanelMode panel(false);
    // Options does nothing: the pre-W16 VR session, scene only.
    for (int frame = 0; frame < 4; ++frame) {
        const viewer::UiRoute route = panel.route(frame % 2 == 0 ? kOptions : 0u);
        CHECK_FALSE(route.panel_visible);
        CHECK_FALSE(route.panel_input);
        CHECK_FALSE(route.opened);
        CHECK(route.scene_controls);
    }
    CHECK_FALSE(panel.open());
    CHECK_FALSE(panel.available());
}

TEST_CASE("vr_ui_state: a panel open at boot ignores buttons held from the launch screen") {
    // UI=1: the session starts in the menu, and the button the title was
    // launched with is still down on the first frame.
    PanelMode panel(true, true);
    const viewer::UiRoute first = panel.route(kCross);
    CHECK(first.panel_visible);
    CHECK(first.panel_input);
    CHECK(first.ui_pressed == 0u);   // not a Select
    CHECK((first.ui_held & splat::Select) != 0u);

    // Let go and press again: that press is the panel's.
    CHECK(panel.route(0).ui_pressed == 0u);
    CHECK((panel.route(kCross).ui_pressed & splat::Select) != 0u);
}

TEST_CASE("vr_ui_state: the left stick moves the panel's focus, not the viewer") {
    PanelMode panel(true);
    open_panel(panel);
    panel.route(0);
    // Past the threshold: a d-pad direction for the panel, no scene controls.
    const viewer::UiRoute right = panel.route(0, 0.9f, 0.0f);
    CHECK((right.ui_pressed & splat::Right) != 0u);
    CHECK_FALSE(right.scene_controls);
    // Held: the direction repeats as ui_held, not as a new press.
    const viewer::UiRoute held = panel.route(0, 0.9f, 0.0f);
    CHECK(held.ui_pressed == 0u);
    CHECK((held.ui_held & splat::Right) != 0u);
    // Inside the dead zone the panel does not move either.
    CHECK(panel.route(0, 0.2f, 0.2f).ui_pressed == 0u);

    // Closed, the same stick is the scene's: the state machine gives it back.
    panel.close();
    const viewer::UiRoute scene = panel.route(0, 0.9f, 0.0f);
    CHECK(scene.scene_controls);
    CHECK(scene.ui_pressed == 0u);
    CHECK(scene.ui_held == 0u);
}

TEST_CASE("vr_ui_state: the panel's page keys are L1/R1, its menu key is Options") {
    // The panel's map is pad::ui_actions_for, one table for pad and script
    // (docs/ui-v1-plan.md section 2); this pins the keys the VR routing exposes.
    PanelMode panel(true);
    open_panel(panel);
    panel.route(0);
    const viewer::UiRoute l1 = panel.route(kL1);
    CHECK((l1.ui_pressed & splat::PagePrev) != 0u);
    const viewer::UiRoute r1 = panel.route(kR1);
    CHECK((r1.ui_pressed & splat::PageNext) != 0u);
    const viewer::UiRoute triangle = panel.route(kTriangle);
    CHECK((triangle.ui_pressed & splat::Tooltips) != 0u);
}

TEST_CASE("vr_ui_state: with UiApp, Options opens Settings and Circle at the root resumes") {
    // The routing above feeding the real panel: what a VR session does when
    // the user opens the panel over a scene and backs out of it again.
    splat::UiServices services;
    splat::UiApp app(services);
    splat::register_default_screens(app.registry());
    app.start();
    app.set_scene_loaded(true);

    PanelMode panel(true);
    open_panel(panel);
    const auto frame = [&](uint32_t buttons) {
        const viewer::UiRoute route = panel.route(buttons);
        splat::UiInput input;
        input.pressed = route.ui_pressed;
        input.held = route.ui_held;
        if (route.panel_input) app.update(input);
        return route;
    };
    frame(0);
    frame(kOptions);
    CHECK(app.nav().top_id() == splat::ScreenId::Settings);
    CHECK_FALSE(app.take_command().has_value());
    frame(0);
    frame(viewer::pad::kCircle);  // Settings -> the folders
    CHECK(app.nav().top_id() == splat::ScreenId::Albums);
    CHECK_FALSE(app.take_command().has_value());
    frame(0);
    frame(viewer::pad::kCircle);  // at the root: back to the scene
    const std::optional<splat::UiCommand> command = app.take_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == splat::UiCommand::ResumeScene);
    panel.close();
    CHECK(frame(0).scene_controls);
}


// ---- 0.6.0: Options resolves on release (OptionsGate) -----------------------
// "Hold Options (~1 s) recentres the scene and the open window, tap Options
// still opens the menu, and a hold never toggles." The gate sits between the
// pads' button word and PanelMode::route(), in the same order vr_mode.cpp calls
// them, with synthetic words and time.

namespace {

using viewer::OptionsGate;

constexpr double kFrame = 1.0 / 60.0;

// One frame of the VR loop's input: the gate, then the panel route.
struct Frame {
    OptionsGate::Step gate;
    viewer::UiRoute route;
};

struct Rig {
    OptionsGate gate;
    PanelMode panel{true};
    double now = 10.0;

    Frame step(uint32_t buttons) {
        Frame frame;
        frame.gate = gate.update(buttons, now);
        frame.route = panel.route(frame.gate.buttons);
        now += kFrame;
        return frame;
    }
    // `seconds` of frames with `buttons` held; returns how many frames each
    // outcome happened on.
    struct Totals {
        int recentres = 0;
        int opened = 0;
        int menu_pressed = 0;
        Frame last;
    };
    Totals hold(uint32_t buttons, double seconds) {
        Totals totals;
        const int frames = static_cast<int>(seconds / kFrame + 0.5);
        for (int i = 0; i < frames; ++i) {
            totals.last = step(buttons);
            totals.recentres += totals.last.gate.recentre ? 1 : 0;
            totals.opened += totals.last.route.opened ? 1 : 0;
            totals.menu_pressed += (totals.last.route.ui_pressed & splat::Menu) != 0u ? 1 : 0;
        }
        return totals;
    }
};

}  // namespace

TEST_CASE("vr_ui_state: an Options tap opens the menu on release, not on press") {
    Rig rig;
    rig.hold(0, 0.2);                       // the gate is armed by a frame without Options
    const Rig::Totals down = rig.hold(kOptions, 0.15);
    CHECK(down.opened == 0);                // pressed, not released: nothing yet
    CHECK(down.last.gate.pending);
    CHECK((down.last.gate.buttons & kOptions) == 0u);
    CHECK_FALSE(rig.panel.open());
    const Rig::Totals up = rig.hold(0, 0.05);
    CHECK(up.opened == 1);                  // the release frame opens it, once
    CHECK(up.recentres == 0);
    CHECK(rig.panel.open());
    CHECK_FALSE(up.last.gate.pending);
    // Nothing repeats afterwards.
    const Rig::Totals after = rig.hold(0, 0.5);
    CHECK(after.opened == 0);
    CHECK(after.recentres == 0);
}

TEST_CASE("vr_ui_state: an Options hold recentres once and never opens or toggles the menu") {
    Rig rig;
    rig.hold(0, 0.2);

    // Panel closed: held 1.5 s, then released.
    const Rig::Totals held = rig.hold(kOptions, 1.5);
    CHECK(held.recentres == 1);
    CHECK(held.opened == 0);
    CHECK_FALSE(rig.panel.open());
    CHECK(held.last.gate.progress == doctest::Approx(1.0f));
    const Rig::Totals released = rig.hold(0, 0.3);
    CHECK(released.opened == 0);            // the release after a hold is silent
    CHECK(released.recentres == 0);
    CHECK_FALSE(rig.panel.open());

    // The next tap works as a tap again (the hold left nothing behind).
    rig.hold(kOptions, 0.1);
    CHECK(rig.hold(0, 0.05).opened == 1);
    CHECK(rig.panel.open());

    // Panel open: a hold is the recentre, and the panel never sees Options - not
    // as a press, not as a held bit - so Settings does not open or close.
    rig.hold(0, 0.2);
    const Rig::Totals open_hold = rig.hold(kOptions, 1.4);
    CHECK(open_hold.recentres == 1);
    CHECK(open_hold.menu_pressed == 0);
    CHECK((open_hold.last.route.ui_held & splat::Menu) == 0u);
    CHECK(open_hold.last.route.panel_visible);
    CHECK(rig.hold(0, 0.3).menu_pressed == 0);
    CHECK(rig.panel.open());

    // Panel open: a tap is the panel's own Options (Settings), once, on release.
    rig.hold(kOptions, 0.1);
    CHECK(rig.hold(0, 0.05).menu_pressed == 1);
}

TEST_CASE("vr_ui_state: the Options hold completes at one second and shows a bar after a quarter") {
    Rig rig;
    rig.hold(0, 0.2);
    // 0.99 s: pending, no recentre yet; the bar follows the progress.
    bool recentred = false;
    float bar_at_100ms = -1.0f;
    float bar_at_500ms = -1.0f;
    for (int i = 0; i < 59; ++i) {
        const Frame frame = rig.step(kOptions);
        recentred = recentred || frame.gate.recentre;
        if (i == 5) bar_at_100ms = frame.gate.bar;
        if (i == 29) bar_at_500ms = frame.gate.bar;
        CHECK(frame.gate.pending);
    }
    CHECK_FALSE(recentred);
    CHECK(bar_at_100ms == doctest::Approx(0.0f));   // a tap must not flash the bar
    CHECK(bar_at_500ms == doctest::Approx(0.5f).epsilon(0.05));
    // Frame 60 reaches kRecentreHoldSeconds (1.0 s) exactly once.
    int recentres = 0;
    for (int i = 0; i < 5; ++i) recentres += rig.step(kOptions).gate.recentre ? 1 : 0;
    CHECK(recentres == 1);
    CHECK(OptionsGate::kBarAfterSeconds < viewer::pad::kRecentreHoldSeconds);
}

TEST_CASE("vr_ui_state: Options from the DualSense and the Sense pad are one press") {
    Rig rig;
    rig.hold(0, 0.2);
    // The word the loop gives the gate is the OR of every pad: the DualSense
    // releases at 0.6 s while the Sense controller (down since 0.4 s) holds on.
    // One press from the first edge: the hold lands at 1.0 s after it, and the
    // other pad's later edge restarts nothing.
    int recentres = 0;
    for (int i = 0; i < 60 * 2; ++i) {
        const double t = i * kFrame;
        const bool dualsense = t < 0.6;
        const bool sense = t >= 0.4;
        const Frame frame = rig.step((dualsense || sense) ? kOptions : 0u);
        recentres += frame.gate.recentre ? 1 : 0;
        if (frame.gate.recentre) CHECK(t == doctest::Approx(1.0).epsilon(0.05));
    }
    CHECK(recentres == 1);
    CHECK_FALSE(rig.panel.open());
}

TEST_CASE("vr_ui_state: a launch press and other buttons pass the Options gate correctly") {
    Rig rig;
    // The title was launched with Options down: it is not a press on the menu,
    // not a tap on release, and not the start of a hold.
    const Rig::Totals launch = rig.hold(kOptions, 1.5);
    CHECK(launch.recentres == 0);
    CHECK(launch.opened == 0);
    CHECK(rig.hold(0, 0.2).opened == 0);
    CHECK_FALSE(rig.panel.open());
    // The first real tap after it works.
    rig.hold(kOptions, 0.1);
    CHECK(rig.hold(0, 0.05).opened == 1);

    // Every other button goes through untouched, with or without Options.
    Rig other;
    other.hold(0, 0.1);
    const Frame mixed = other.step(kCross | kL1 | kOptions);
    CHECK((mixed.gate.buttons & kCross) != 0u);
    CHECK((mixed.gate.buttons & kL1) != 0u);
    CHECK((mixed.gate.buttons & kOptions) == 0u);

    // UI=0: no panel, and the hold still reports a recentre (the scene's).
    OptionsGate gate;
    PanelMode no_panel(false);
    double t = 0.0;
    int recentres = 0;
    gate.update(0, t);
    for (int i = 0; i < 100; ++i) {
        t += kFrame;
        const OptionsGate::Step step = gate.update(kOptions, t);
        recentres += step.recentre ? 1 : 0;
        const viewer::UiRoute route = no_panel.route(step.buttons);
        CHECK_FALSE(route.panel_visible);
        CHECK(route.scene_controls);
    }
    CHECK(recentres == 1);
}
