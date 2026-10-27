// ScreenStack: push/pop/replace, on_enter on every reveal, the root rule (the
// stack never empties), the "not available yet" report and delegation to the
// top screen.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui/screen_stack.h"
#include "ui/theme.h"

namespace {

class StubScreen : public splat::Screen {
public:
    explicit StubScreen(std::string name, bool overlay = false)
        : name_(std::move(name)), overlay_(overlay) {}
    bool overlay() const override { return overlay_; }

    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override {
        ++enters;
        last_a = args.a;
    }
    void update(splat::UiContext&, const splat::UiInput&) override { ++updates; }
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override { ++draws; }
    std::vector<splat::Hint> hints() const override { return {splat::Hint{"Cross", "open"}}; }
    std::string title() const override { return name_; }

    int enters = 0;
    int updates = 0;
    int draws = 0;
    std::string last_a;

private:
    std::string name_;
    bool overlay_ = false;
};

struct Fixture {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> unavailable;

    Fixture() {
        stack.set_context(&context);
        stack.set_unavailable_handler(
            [this](const std::string& name) { unavailable.push_back(name); });
    }
};

}  // namespace

TEST_CASE("ui_screen_stack push pop and replace") {
    Fixture fixture;
    StubScreen* photos = nullptr;
    fixture.registry.add(splat::ScreenId::Photos, [&photos] {
        auto screen = std::make_unique<StubScreen>("Photos");
        photos = screen.get();
        return screen;
    });
    fixture.registry.add(splat::ScreenId::Scenes,
                        [] { return std::make_unique<StubScreen>("Scenes"); });

    splat::ScreenArgs args;
    args.a = "2026-10-26";
    CHECK(fixture.stack.push(splat::ScreenId::Photos, args));
    REQUIRE(fixture.stack.size() == 1);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Photos);
    REQUIRE(photos != nullptr);
    CHECK(photos->enters == 1);
    CHECK(photos->last_a == "2026-10-26");
    CHECK(fixture.stack.title() == "Photos");

    CHECK(fixture.stack.push(splat::ScreenId::Scenes));
    CHECK(fixture.stack.size() == 2);
    CHECK(fixture.stack.title() == "Scenes");
    const std::vector<std::string> titles = fixture.stack.titles();
    REQUIRE(titles.size() == 2);
    CHECK(titles[0] == "Photos");
    CHECK(titles[1] == "Scenes");

    // A pop re-enters the revealed screen with the arguments it was pushed
    // with, so it can re-list the filesystem.
    CHECK(fixture.stack.pop());
    CHECK(fixture.stack.size() == 1);
    CHECK(photos->enters == 2);
    CHECK(photos->last_a == "2026-10-26");

    // replace swaps the top in place.
    CHECK(fixture.stack.replace(splat::ScreenId::Scenes));
    CHECK(fixture.stack.size() == 1);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);
}

TEST_CASE("ui_screen_stack never pops the root") {
    Fixture fixture;
    fixture.registry.add(splat::ScreenId::Settings,
                         [] { return std::make_unique<StubScreen>("Menu"); });
    CHECK(fixture.stack.push(splat::ScreenId::Settings));
    CHECK_FALSE(fixture.stack.pop());
    CHECK(fixture.stack.size() == 1);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
}

TEST_CASE("ui_screen_stack reports an unregistered screen") {
    Fixture fixture;
    fixture.registry.add(splat::ScreenId::Settings,
                         [] { return std::make_unique<StubScreen>("Menu"); });
    REQUIRE(fixture.stack.push(splat::ScreenId::Settings));

    CHECK_FALSE(fixture.stack.push(splat::ScreenId::Queue));
    CHECK(fixture.stack.size() == 1);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    REQUIRE(fixture.unavailable.size() == 1);
    CHECK(fixture.unavailable[0] == "Conversions");

    // replace on an unregistered id leaves the current screen alone.
    CHECK_FALSE(fixture.stack.replace(splat::ScreenId::Models));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(fixture.unavailable.size() == 2);
}

TEST_CASE("ui_screen_stack delegates to the top") {
    Fixture fixture;
    StubScreen* screen = nullptr;
    fixture.registry.add(splat::ScreenId::Settings, [&screen] {
        auto created = std::make_unique<StubScreen>("Menu");
        screen = created.get();
        return created;
    });
    REQUIRE(fixture.stack.push(splat::ScreenId::Settings));
    REQUIRE(screen != nullptr);

    splat::UiCanvas canvas(64, 64);
    fixture.stack.update(fixture.context, splat::UiInput{});
    fixture.stack.draw(fixture.context, canvas, splat::Rect{0, 0, 64, 64});
    CHECK(screen->updates == 1);
    CHECK(screen->draws == 1);

    const std::vector<splat::Hint> hints = fixture.stack.hints();
    REQUIRE(hints.size() == 1);
    CHECK(hints[0].button == "Cross");

    // An empty stack has no title and no hints.
    Fixture empty_fixture;
    CHECK(empty_fixture.stack.empty());
    CHECK(empty_fixture.stack.title().empty());
    CHECK(empty_fixture.stack.hints().empty());
}

TEST_CASE("ui_screen_stack pop_to, find and overlay dialogs") {
    Fixture fixture;
    std::vector<StubScreen*> made;
    const auto add = [&](splat::ScreenId id, const char* name, bool overlay) {
        fixture.registry.add(id, [&made, name, overlay] {
            auto screen = std::make_unique<StubScreen>(name, overlay);
            made.push_back(screen.get());
            return screen;
        });
    };
    add(splat::ScreenId::Albums, "Scenes", false);
    add(splat::ScreenId::Scenes, "trip", false);
    add(splat::ScreenId::Photos, "Add photos", false);
    add(splat::ScreenId::Keyboard, "Delete", true);
    REQUIRE(fixture.stack.push(splat::ScreenId::Albums));
    REQUIRE(fixture.stack.push(splat::ScreenId::Scenes));
    REQUIRE(fixture.stack.push(splat::ScreenId::Photos));
    CHECK(fixture.stack.find(splat::ScreenId::Scenes) == 1);
    CHECK(fixture.stack.find(splat::ScreenId::Settings) == -1);

    // pop_to leaves `depth` screens and enters only the one revealed.
    StubScreen* folder = made[1];
    const int enters = folder->enters;
    CHECK(fixture.stack.pop_to(2));
    CHECK(fixture.stack.size() == 2);
    CHECK(folder->enters == enters + 1);
    CHECK_FALSE(fixture.stack.pop_to(2));
    // Depth 0 is treated as 1: the root always stays.
    CHECK(fixture.stack.pop_to(0));
    CHECK(fixture.stack.size() == 1);

    // An overlay is drawn over the screen below it and keeps that title.
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard));
    StubScreen* dialog = made.back();
    StubScreen* below = made[0];
    const int below_draws = below->draws;
    splat::UiCanvas canvas(64, 64);
    fixture.stack.draw(fixture.context, canvas, splat::Rect{0, 0, 64, 64});
    CHECK(dialog->draws == 1);
    CHECK(below->draws == below_draws + 1);
    CHECK(fixture.stack.title() == "Scenes");
    CHECK(fixture.stack.hints()[0].label == "open");
}

