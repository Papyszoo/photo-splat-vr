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

