// SettingsScreen: the row order, the live conversion status (the same chip as
// the top bar) and "n ready" text, the dim rule for a missing service, the
// rows' targets and the prompts.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "common/path.h"
#include "ui/fakes.h"
#include "ui/screens/settings.h"
#include "ui/screen_stack.h"

namespace {

class StubScreen : public splat::Screen {
public:
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

struct Fixture {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::SettingsScreen settings;

    Fixture() { settings.on_enter(context, splat::ScreenArgs{}); }
};

}  // namespace

TEST_CASE("ui_settings lists the rows in order") {
    Fixture fixture;
    const std::vector<splat::ListRow>& rows = fixture.settings.list().rows();
    REQUIRE(rows.size() == splat::SettingsScreen::ItemCount);
    CHECK(rows[splat::SettingsScreen::ComfortHeader].header);
    CHECK(rows[splat::SettingsScreen::ComfortHeader].label == "Comfort");
    CHECK(rows[splat::SettingsScreen::ScaleAround].label == "Scale");
    CHECK(rows[splat::SettingsScreen::MoveSpeed].label == "Move speed");
    CHECK(rows[splat::SettingsScreen::TurnSpeed].label == "Turn speed");
    CHECK(rows[splat::SettingsScreen::DisplayHeader].header);
    CHECK(rows[splat::SettingsScreen::DisplayHeader].label == "Display");
    CHECK(rows[splat::SettingsScreen::EdgeSoftness].label == "Edge softness");
    CHECK(rows[splat::SettingsScreen::Blending].label == "Blending");
    CHECK(rows[splat::SettingsScreen::SystemHeader].header);
    CHECK(rows[splat::SettingsScreen::Models].label == "Models");
    CHECK(rows[splat::SettingsScreen::Conversions].label == "Conversions");
    CHECK(rows[splat::SettingsScreen::Quit].label == "Quit");
    CHECK(fixture.settings.title() == "Settings");
    // The first frame's focus is the first real row, not the heading.
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::ScaleAround);

    const std::vector<splat::Hint> hints = fixture.settings.hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "Change");
    CHECK(hints[1].button == "Circle");
    CHECK(hints[1].label == "Back");
}

TEST_CASE("ui_settings dims rows whose service is missing") {
    Fixture fixture;
    const std::vector<splat::ListRow>& rows = fixture.settings.list().rows();
    CHECK(rows[splat::SettingsScreen::Models].dim);
    CHECK(rows[splat::SettingsScreen::Models].right.empty());
    CHECK(rows[splat::SettingsScreen::Conversions].dim);
    CHECK_FALSE(rows[splat::SettingsScreen::Quit].dim);
    CHECK_FALSE(rows[splat::SettingsScreen::ScaleAround].dim);
}

TEST_CASE("ui_settings shows the ready model count") {
    splat::FakeJobService jobs;
    REQUIRE(jobs.enqueue({splat::Path("a.jpg")}, "sharp", "").ok());
    jobs.tick(1.0);
    splat::FakeModelService models;  // DA2-Small starts ready

    Fixture fixture;
    fixture.services.jobs = &jobs;
    fixture.services.models = &models;
    fixture.settings.on_enter(fixture.context, splat::ScreenArgs{});

    const std::vector<splat::ListRow>& rows = fixture.settings.list().rows();
    CHECK_FALSE(rows[splat::SettingsScreen::Conversions].dim);
    // The conversion status is the top bar's chip, not a second copy here.
    CHECK(rows[splat::SettingsScreen::Conversions].right.empty());
    CHECK_FALSE(rows[splat::SettingsScreen::Models].dim);
    CHECK(rows[splat::SettingsScreen::Models].right == "1 ready");

    // No model ready says so, on the next frame without an on_enter.
    models.remove("da2");
    fixture.settings.update(fixture.context, splat::UiInput{});
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::Models].right == "None installed");
    // The live refresh does not disturb the focus.
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::ScaleAround);
}

TEST_CASE("ui_settings rows open their screens and quit") {
    Fixture fixture;
    fixture.registry.add(splat::ScreenId::Settings, [] { return std::make_unique<StubScreen>(); });
    fixture.registry.add(splat::ScreenId::Models, [] { return std::make_unique<StubScreen>(); });
    fixture.registry.add(splat::ScreenId::Queue, [] { return std::make_unique<StubScreen>(); });
    REQUIRE(fixture.stack.push(splat::ScreenId::Settings));
    std::vector<splat::UiCommand> commands;
    fixture.context.command_sink = [&commands](splat::UiCommand command) {
        commands.push_back(command);
    };

    // From ScaleAround down to Models: the three comfort rows and the two
    // display rows, with the headings skipped by ListView.
    for (int i = 0; i < 5; ++i) fixture.settings.update(fixture.context, press(splat::Down));
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::Models);
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Models);
    fixture.stack.pop();

    fixture.settings.update(fixture.context, press(splat::Down));
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Queue);
    fixture.stack.pop();

    fixture.settings.update(fixture.context, press(splat::Down));
    fixture.settings.update(fixture.context, press(splat::Select));
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].kind == splat::UiCommand::Quit);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
}

TEST_CASE("ui_settings cycles the comfort and display values") {
    Fixture fixture;
    // Scale: Maquette (the default) -> Front -> Centre by Cross (or Right).
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::ScaleAround].right == "Maquette");
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.settings.values().scale_around == 0);
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::ScaleAround].right == "Front");
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.settings.values().scale_around == 1);
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::ScaleAround].right == "Centre");
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.settings.values().scale_around == 2);
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::ScaleAround].right == "Maquette");

    // Move speed: 2.5 -> 4.0 m/s with Right, and back with Left.
    fixture.settings.update(fixture.context, press(splat::Down));
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::MoveSpeed);
    CHECK(fixture.settings.values().move_speed == doctest::Approx(2.5f));
    fixture.settings.update(fixture.context, press(splat::Right));
    CHECK(fixture.settings.values().move_speed == doctest::Approx(4.0f));
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::MoveSpeed].right == "4 m/s");
    fixture.settings.update(fixture.context, press(splat::Left));
    CHECK(fixture.settings.values().move_speed == doctest::Approx(2.5f));

    // Turn speed: 40 -> 60 deg/s.
    fixture.settings.update(fixture.context, press(splat::Down));
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::TurnSpeed);
    fixture.settings.update(fixture.context, press(splat::Right));
    CHECK(fixture.settings.values().turn_speed == doctest::Approx(60.0f));
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::TurnSpeed].right == "60 deg/s");

    // Edge softness and blending are two-state toggles.
    fixture.settings.update(fixture.context, press(splat::Down));
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::EdgeSoftness);
    CHECK(fixture.settings.values().edge_softness == doctest::Approx(0.3f));
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK(fixture.settings.values().edge_softness == doctest::Approx(0.5f));
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::EdgeSoftness].right ==
          "Soft (0.5)");
    fixture.settings.update(fixture.context, press(splat::Down));
    CHECK(fixture.settings.list().focus() == splat::SettingsScreen::Blending);
    CHECK(fixture.settings.values().blend_linear);
    fixture.settings.update(fixture.context, press(splat::Select));
    CHECK_FALSE(fixture.settings.values().blend_linear);
    CHECK(fixture.settings.list().rows()[splat::SettingsScreen::Blending].right == "sRGB");
}
