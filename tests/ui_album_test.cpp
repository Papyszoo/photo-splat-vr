// AlbumPickScreen ("Move to folder"): the album rows (the implicit "All
// scenes" first, "New folder" last and no scene deletion), moving a scene with move_scene(), the
// refusal when the destination already holds a folder of the same name, and
// the keyboard push for a new album, the shared scene-count label. Also covers
// the delete helper the keyboard uses: a non-empty album is refused, an empty
// one is removed.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/gallery.h"
#include "test_util.h"
#include "ui/screens/album_pick.h"
#include "ui/screens/albums.h"
#include "ui/screens/keyboard.h"
#include "ui/screen_stack.h"

using splat::Path;

namespace {

class CapturingScreen : public splat::Screen {
public:
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override {
        last = args;
        ++enters;
    }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }

    splat::ScreenArgs last;
    int enters = 0;
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

void make_scene(const Path& folder) {
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "scene.psplat") << "scene";
}

struct Fixture {
    splat_test::TempDir temp{"ui-album"};
    Path root = temp.path() / "scenes";
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    splat::AlbumPickScreen* pick = nullptr;
    CapturingScreen* keyboard = nullptr;

    Fixture() {
        std::filesystem::create_directories(root);
        services.paths.scenes = root;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        stack.set_context(&context);

        registry.add(splat::ScreenId::Settings, [] { return std::make_unique<CapturingScreen>(); });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            keyboard = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::AlbumPick, [this] {
            auto screen = std::make_unique<splat::AlbumPickScreen>();
            pick = screen.get();
            return screen;
        });
        REQUIRE(stack.push(splat::ScreenId::Settings));
    }

    void enter_move(const Path& scene_folder) {
        splat::ScreenArgs args;
        args.a = "move-scene";
        args.b = scene_folder.string();
        REQUIRE(stack.push(splat::ScreenId::AlbumPick, args));
        REQUIRE(pick != nullptr);
    }

    void tap(uint32_t action) { stack.update(context, press(action)); }
};

}  // namespace

TEST_CASE("ui_album_move lists albums and moves the scene on select") {
    Fixture fixture;
    make_scene(fixture.root / "s1");
    std::filesystem::create_directories(fixture.root / "trip");
    fixture.enter_move(fixture.root / "s1");

    CHECK(fixture.pick->title() == "Move to folder");
    CHECK(fixture.pick->scene_folder() == (fixture.root / "s1").string());

    // The scene is loose: Unsorted (where it is now) is not offered.
    const std::vector<std::string> names = fixture.pick->names();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "trip");
    CHECK(names[1] == "New folder");
    for (const std::string& name : names) {
        CHECK(name.find("Delete") == std::string::npos);
    }

    // "New folder" is an action row: a "+" and no date line.
    const std::vector<splat::ListRow>& rows = fixture.pick->list().rows();
    CHECK(rows[0].sublabel == "0 scenes");
    CHECK_FALSE(rows[0].add);
    CHECK(rows[1].add);
    CHECK(rows[1].sublabel.empty());

    fixture.pick->focus_row(0);
    fixture.tap(splat::Select);

    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s1"));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "s1"));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("Moved to trip") != std::string::npos);
}

TEST_CASE("ui_album_move refuses to overwrite an existing scene folder") {
    Fixture fixture;
    make_scene(fixture.root / "s1");
    make_scene(fixture.root / "trip" / "s1");
    fixture.enter_move(fixture.root / "s1");

    fixture.pick->focus_row(0);  // trip
    fixture.tap(splat::Select);

    CHECK(fixture.stack.top_id() == splat::ScreenId::Settings);
    CHECK(std::filesystem::is_directory(fixture.root / "s1"));
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s1"));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("already exists") != std::string::npos);
}

TEST_CASE("ui_album_pick New folder pushes the keyboard with a default name") {
    Fixture fixture;
    make_scene(fixture.root / "s1");
    std::filesystem::create_directories(fixture.root / "trip");
    fixture.enter_move(fixture.root / "s1");

    REQUIRE(fixture.pick->names().size() == 2);
    fixture.pick->focus_row(1);
    fixture.tap(splat::Select);

    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);
    CHECK(fixture.keyboard->enters == 1);
    CHECK(fixture.keyboard->last.a == "new-album");
    const std::string expected = splat::default_album_name(std::time(nullptr), fixture.root);
    REQUIRE(fixture.keyboard->last.b.size() >= 10);
    CHECK(fixture.keyboard->last.b.substr(0, 10) == expected.substr(0, 10));
}

TEST_CASE("ui_album_move does not offer the scene's own folder") {
    Fixture fixture;
    make_scene(fixture.root / "trip" / "s1");
    std::filesystem::create_directories(fixture.root / "garden");
    fixture.enter_move(fixture.root / "trip" / "s1");
    const std::vector<std::string> names = fixture.pick->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Unsorted");
    CHECK(names[1] == "garden");
    CHECK(names[2] == "New folder");
}

TEST_CASE("ui_album scene count labels come from the shared helper") {
    // One definition, declared in albums.h, used by the album grid and the
    // album-pick rows.
    CHECK(splat::scene_count_label(0) == "0 scenes");
    CHECK(splat::scene_count_label(1) == "1 scene");
    CHECK(splat::scene_count_label(2) == "2 scenes");
}

TEST_CASE("ui_album delete helper refuses a non-empty album and deletes an empty one") {
    Fixture fixture;
    make_scene(fixture.root / "full" / "s1");
    const splat::Status full = splat::delete_album_checked(fixture.root, "full");
    CHECK_FALSE(full.ok());
    CHECK(full.message().find("not empty") != std::string::npos);
    CHECK(std::filesystem::is_directory(fixture.root / "full" / "s1"));

    std::filesystem::create_directories(fixture.root / "empty");
    CHECK(splat::delete_album_checked(fixture.root, "empty").ok());
    CHECK_FALSE(std::filesystem::exists(fixture.root / "empty"));

    // The loose-scene album and a missing album are refused.
    CHECK_FALSE(splat::delete_album_checked(fixture.root, "").ok());
    CHECK_FALSE(splat::delete_album_checked(fixture.root, "missing").ok());

    // A folder that has no scene but holds another file is not "empty".
    std::filesystem::create_directories(fixture.root / "stray");
    std::ofstream(fixture.root / "stray" / "note.txt") << "x";
    const splat::Status stray = splat::delete_album_checked(fixture.root, "stray");
    CHECK_FALSE(stray.ok());
    CHECK(stray.message().find("not empty") != std::string::npos);
    CHECK(std::filesystem::is_directory(fixture.root / "stray"));
}
