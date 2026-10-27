// UiApp: the chrome contract (title, button prompts, toast), the navigation
// rules (Circle asks the screen, then pops; at the root it resumes a loaded
// scene; Options opens and closes Settings), the Quit row, the dirty rule (an
// idle frame draws nothing) and the tooltips dim flag.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ui/app.h"
#include "ui/fakes.h"
#include "ui/screens/albums.h"
#include "ui/screens/registry.h"

namespace {

class StubScreen : public splat::Screen {
public:
    explicit StubScreen(std::string title, bool keeps_back = false)
        : title_(std::move(title)), keeps_back_(keeps_back) {}
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return title_; }
    bool handle_back(splat::UiContext&) override {
        if (!keeps_back_) return false;
        ++backs;
        return true;
    }
    int backs = 0;

private:
    std::string title_;
    bool keeps_back_;
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

std::uint64_t checksum(const splat::UiCanvas& canvas, int y, int height) {
    std::uint64_t sum = 0;
    const std::size_t begin = static_cast<std::size_t>(y) * canvas.width() * 4;
    const std::size_t end = begin + static_cast<std::size_t>(height) * canvas.width() * 4;
    for (std::size_t index = begin; index < end && index < canvas.pixels().size(); ++index) {
        sum = sum * 131 + canvas.pixels()[index];
    }
    return sum;
}

struct Fixture {
    splat::UiServices services;
    splat::UiApp app{services};
    splat::UiCanvas canvas{1280, 720};

    // all_screens=false registers only the root, so Settings is unavailable.
    explicit Fixture(bool all_screens = true) {
        if (all_screens) {
            register_default_screens(app.registry());
        } else {
            app.registry().add(splat::ScreenId::Albums,
                               [] { return std::make_unique<splat::AlbumsScreen>(); });
        }
    }
};

}  // namespace

TEST_CASE("ui_app starts at the folders and back pops to them") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(splat::UiInput{});
    REQUIRE(fixture.app.nav().size() == 1);
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Albums);
    CHECK(fixture.app.nav().title() == "Scenes");

    // Circle at the root with no scene loaded does nothing.
    fixture.app.update(press(splat::Back));
    CHECK(fixture.app.nav().size() == 1);
    CHECK_FALSE(fixture.app.take_command().has_value());

    fixture.app.nav().push(splat::ScreenId::Scenes);
    CHECK(fixture.app.nav().size() == 2);
    fixture.app.update(press(splat::Back));
    CHECK(fixture.app.nav().size() == 1);
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Albums);
}

TEST_CASE("ui_app circle at the root resumes a loaded scene") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.set_scene_loaded(true);

    // Deeper in, Circle only pops.
    fixture.app.nav().push(splat::ScreenId::Scenes);
    fixture.app.update(press(splat::Back));
    CHECK(fixture.app.nav().size() == 1);
    CHECK_FALSE(fixture.app.take_command().has_value());

    fixture.app.update(press(splat::Back));
    const std::optional<splat::UiCommand> command = fixture.app.take_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == splat::UiCommand::ResumeScene);
    CHECK_FALSE(fixture.app.take_command().has_value());
    CHECK(fixture.app.nav().size() == 1);
}

TEST_CASE("ui_app a screen can keep circle for itself") {
    Fixture fixture;
    StubScreen* stub = nullptr;
    fixture.app.registry().add(splat::ScreenId::Photos, [&stub] {
        auto screen = std::make_unique<StubScreen>("Add photos", true);
        stub = screen.get();
        return screen;
    });
    fixture.app.start();
    fixture.app.nav().push(splat::ScreenId::Photos);
    fixture.app.update(press(splat::Back));
    REQUIRE(stub != nullptr);
    CHECK(stub->backs == 1);
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Photos);
}

TEST_CASE("ui_app options opens and closes settings") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.nav().push(splat::ScreenId::Scenes);

    fixture.app.update(press(splat::Menu));
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Settings);
    CHECK(fixture.app.nav().size() == 3);
    // Options never resumes the scene: that is Circle at the root.
    fixture.app.set_scene_loaded(true);

    // Into Models, then Options closes Settings and everything above it.
    for (int i = 0; i < 5; ++i) fixture.app.update(press(splat::Down));
    fixture.app.update(press(splat::Select));
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Models);
    fixture.app.update(press(splat::Menu));
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Scenes);
    CHECK(fixture.app.nav().size() == 2);
    CHECK_FALSE(fixture.app.take_command().has_value());
}

TEST_CASE("ui_app quit is a settings row") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(press(splat::Menu));
    for (int i = 0; i < 7; ++i) fixture.app.update(press(splat::Down));
    fixture.app.update(press(splat::Select));

    const std::optional<splat::UiCommand> command = fixture.app.take_command();
    REQUIRE(command.has_value());
    CHECK(command->kind == splat::UiCommand::Quit);
    // Quit does not navigate.
    CHECK(fixture.app.nav().top_id() == splat::ScreenId::Settings);
}

TEST_CASE("ui_app unavailable screen toasts and stays put") {
    Fixture fixture(false);
    fixture.app.start();
    fixture.app.update(splat::UiInput{});

    // Only the root is registered in this fixture, so Settings is unavailable.
    fixture.app.update(press(splat::Menu));
    CHECK(fixture.app.nav().size() == 1);
    CHECK(fixture.app.toast_visible());
    CHECK(fixture.app.toast_text().find("not available yet") != std::string::npos);
    CHECK(fixture.app.toast_text().find("Settings") != std::string::npos);
}

TEST_CASE("ui_app idle frames draw nothing") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(splat::UiInput{});
    CHECK(fixture.app.draw(fixture.canvas));
    CHECK_FALSE(fixture.app.draw(fixture.canvas));

    fixture.app.update(splat::UiInput{});
    CHECK_FALSE(fixture.app.draw(fixture.canvas));

    // Any input dirties the frame.
    fixture.app.update(press(splat::Down));
    CHECK(fixture.app.draw(fixture.canvas));
    CHECK_FALSE(fixture.app.draw(fixture.canvas));

    // A moving pointer (the Sense laser) dirties the frame; a still one does
    // not.
    splat::UiInput laser;
    laser.pointer_valid = true;
    laser.px = 100.0f;
    laser.py = 100.0f;
    fixture.app.update(laser);
    CHECK(fixture.app.draw(fixture.canvas));
    fixture.app.update(laser);
    CHECK_FALSE(fixture.app.draw(fixture.canvas));
    laser.px = 120.0f;
    fixture.app.update(laser);
    CHECK(fixture.app.draw(fixture.canvas));

    // Async work (a thumbnail) asks for a repaint through the context.
    fixture.app.context().invalidate();
    CHECK(fixture.app.draw(fixture.canvas));
    CHECK_FALSE(fixture.app.draw(fixture.canvas));
}

TEST_CASE("ui_app repaints a running job about four times a second") {
    splat::FakeJobService jobs;
    splat::UiServices services;
    services.jobs = &jobs;
    splat::UiApp app(services);
    register_default_screens(app.registry());
    app.start();

    splat::UiInput frame;
    frame.dt = 1.0 / 60.0;
    app.update(frame);
    splat::UiCanvas canvas(1280, 720);
    REQUIRE(app.draw(canvas));
    CHECK_FALSE(app.draw(canvas));

    REQUIRE(jobs.enqueue({splat::Path("a.jpg")}, "sharp", "").ok());
    app.update(frame);  // busy transition repaints once
    CHECK(app.draw(canvas));
    CHECK_FALSE(app.draw(canvas));

    int draws = 0;
    for (int step = 0; step < 16; ++step) {  // just over 0.25 s
        app.update(frame);
        if (app.draw(canvas)) ++draws;
    }
    CHECK(draws == 1);
}

TEST_CASE("ui_app toast expires and redraws once") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(splat::UiInput{});
    REQUIRE(fixture.app.draw(fixture.canvas));

    fixture.app.toast("saved", 0.5);
    CHECK(fixture.app.toast_visible());
    CHECK(fixture.app.draw(fixture.canvas));

    splat::UiInput tick;
    tick.dt = 0.25;
    fixture.app.update(tick);
    CHECK(fixture.app.toast_visible());
    CHECK_FALSE(fixture.app.draw(fixture.canvas));  // still visible, no change

    tick.dt = 0.3;
    fixture.app.update(tick);
    CHECK_FALSE(fixture.app.toast_visible());
    CHECK(fixture.app.toast_text().empty());
    CHECK(fixture.app.draw(fixture.canvas));  // the plate disappeared
    CHECK_FALSE(fixture.app.draw(fixture.canvas));
}

TEST_CASE("ui_app tooltips flag dims the hint bar") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(splat::UiInput{});
    REQUIRE(fixture.app.draw(fixture.canvas));
    const std::uint64_t bright = checksum(fixture.canvas, 720 - 76, 76);

    fixture.app.set_tooltips(false);
    CHECK_FALSE(fixture.app.tooltips());
    CHECK(fixture.app.draw(fixture.canvas));
    const std::uint64_t dim = checksum(fixture.canvas, 720 - 76, 76);
    CHECK(bright != dim);

    // Setting the same value does not dirty the frame.
    fixture.app.set_tooltips(false);
    CHECK_FALSE(fixture.app.draw(fixture.canvas));
    fixture.app.set_tooltips(true);
    CHECK(fixture.app.draw(fixture.canvas));
}

TEST_CASE("ui_app scene flag dirties once") {
    Fixture fixture;
    fixture.app.start();
    fixture.app.update(splat::UiInput{});
    REQUIRE(fixture.app.draw(fixture.canvas));
    CHECK_FALSE(fixture.app.draw(fixture.canvas));

    fixture.app.set_scene_loaded(true);
    CHECK(fixture.app.draw(fixture.canvas));
    fixture.app.set_scene_loaded(true);
    CHECK_FALSE(fixture.app.draw(fixture.canvas));
}
