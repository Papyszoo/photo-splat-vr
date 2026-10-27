// ScenesScreen: listing order, the tile line (model, splat count, seconds), the
// album title, ViewScene with the album and index, Square -> AlbumPick args,
// the focus restore on return, and the labelled placeholder a scene without a
// thumbnail gets instead of an error.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/path.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/placeholder.h"
#include "ui/screens/scenes.h"
#include "ui/screen_stack.h"

using splat::Path;

namespace {

void write_scene(const Path& folder, const char* backend, size_t splats, double seconds,
                 bool thumb) {
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = backend;
    meta.seconds_taken = seconds;
    const splat::SplatScene scene = splat_test::make_test_scene(splats);
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::Status status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
    if (!thumb) std::filesystem::remove(splat::scene_files(folder).thumbnail);
}

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
    return input;
}

struct Fixture {
    splat_test::TempDir temp{"ui-scenes"};
    Path root = temp.path() / "scenes";
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<splat::UiCommand> commands;
    splat::ScenesScreen* scenes = nullptr;
    CapturingScreen* album_pick = nullptr;
    CapturingScreen* photos = nullptr;
    CapturingScreen* keyboard = nullptr;

    Fixture() {
        std::filesystem::create_directories(root);
        services.paths.scenes = root;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.command_sink = [this](splat::UiCommand command) {
            commands.push_back(std::move(command));
        };
        stack.set_context(&context);

        registry.add(splat::ScreenId::Scenes, [this] {
            auto screen = std::make_unique<splat::ScenesScreen>();
            scenes = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::AlbumPick, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            album_pick = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Photos, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            photos = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            keyboard = screen.get();
            return screen;
        });
    }

    void enter(const std::string& album) {
        splat::ScreenArgs args;
        args.a = album;
        REQUIRE(stack.push(splat::ScreenId::Scenes, args));
    }
};

}  // namespace

TEST_CASE("ui_scenes lists an album in folder order with model, splats and seconds") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "a-kitchen", "sharp", 1234, 12.5, false);
    write_scene(fixture.root / "trip" / "b-pizza", "depth-anything-v2-small", 213, 0.4, false);
    write_scene(fixture.root / "trip" / "c-garden-night", "sharp", 5, 3.0, true);
    write_scene(fixture.root / "loose", "sharp", 12, 1.0, false);
    fixture.enter("trip");

    CHECK(fixture.scenes->album() == "trip");
    CHECK(fixture.scenes->title() == "trip");

    const std::vector<std::string> names = fixture.scenes->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "a-kitchen");
    CHECK(names[1] == "b-pizza");
    CHECK(names[2] == "c-garden-night");

    // The "+" tile comes first, then the scenes; focus starts on the first scene.
    const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
    REQUIRE(cells.size() == 4);
    CHECK(cells[0].icon == splat::GridCell::Icon::Add);
    CHECK(cells[0].caption == "Add photos");
    CHECK(fixture.scenes->first_scene_cell() == 1);
    CHECK(fixture.scenes->grid().focus() == 1);
    CHECK(fixture.scenes->focused_scene() == 0);
    CHECK(cells[1].caption == "a-kitchen");
    // The scene's info is the model only (users compare models; counts and
    // timings stay out of the shelf), spelled as the Models screen does, and
    // drawn under the focused scene only.
    CHECK(cells[1].sublabel == "SHARP");
    CHECK(cells[2].sublabel == "DA2-Small");
    CHECK(cells[3].sublabel == "SHARP");
    CHECK(fixture.scenes->grid().sublabel_focus_only);
    // A long name of the focused scene gets three lines in a fixed label area.
    CHECK(fixture.scenes->grid().caption_lines == 3);

    const std::vector<splat::Hint> hints = fixture.scenes->hints();
    REQUIRE(hints.size() == 3);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "View");
    CHECK(hints[1].button == "Triangle");
    CHECK(hints[1].label == "More");
    CHECK(hints[2].button == "Circle");
    CHECK(hints[2].label == "Back");
}

TEST_CASE("ui_scenes titles the loose scene album Unsorted") {
    Fixture fixture;
    write_scene(fixture.root / "loose", "sharp", 12, 1.0, false);
    fixture.enter("");

    CHECK(fixture.scenes->album().empty());
    CHECK(fixture.scenes->title() == "Unsorted");
    REQUIRE(fixture.scenes->names().size() == 1);
}

TEST_CASE("ui_scenes select emits ViewScene with the album and index") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 2.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.commands.size() == 1);
    CHECK(fixture.commands[0].kind == splat::UiCommand::ViewScene);
    CHECK(fixture.commands[0].album == "trip");
    CHECK(fixture.commands[0].index == 1);
    // Viewing never navigates the stack by itself: the host owns the scene.
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);

    // The loose scenes travel as the empty album key, with their own index.
    Fixture loose;
    write_scene(loose.root / "a", "sharp", 4, 1.0, false);
    write_scene(loose.root / "b", "sharp", 4, 1.0, false);
    loose.enter("");
    loose.stack.update(loose.context, press(splat::Select));
    REQUIRE(loose.commands.size() == 1);
    CHECK(loose.commands[0].album.empty());
    CHECK(loose.commands[0].index == 0);
}

TEST_CASE("ui_scenes triangle asks to move the focused scene") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Context));
    // The scene's menu ("More"): Move to folder, Rename, Delete.
    REQUIRE(fixture.scenes->menu().is_open());
    REQUIRE(fixture.scenes->menu().items().size() == 3);
    CHECK(fixture.scenes->menu().items()[0] == "Move to folder");
    CHECK(fixture.scenes->menu().items()[1] == "Rename");
    CHECK(fixture.scenes->menu().items()[2] == "Delete");
    CHECK(fixture.scenes->menu().title() == "s2");
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.album_pick != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::AlbumPick);
    CHECK(fixture.album_pick->enters == 1);
    CHECK(fixture.album_pick->last.a == "move-scene");
    CHECK(fixture.album_pick->last.b == (fixture.root / "trip" / "s2").string());
    // Triangle alone does not view the scene.
    CHECK(fixture.commands.empty());
}

TEST_CASE("ui_scenes renames and deletes a scene from its menu") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    // Rename opens the keyboard for that scene folder.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);
    CHECK(fixture.keyboard->last.a == "rename-scene");
    CHECK(fixture.keyboard->last.b == (fixture.root / "trip" / "s1").string());
    REQUIRE(fixture.stack.pop());

    // Delete asks first, with Cancel focused: Cross there keeps the scene.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.scenes->confirm().is_open());
    CHECK(fixture.scenes->confirm().title() == "Delete “s1”?");
    CHECK(fixture.scenes->confirm().focus() == 0);
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(fixture.scenes->confirm().is_open());
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s1"));

    // Circle closes the card too.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.scenes->handle_back(fixture.context));
    CHECK_FALSE(fixture.scenes->confirm().is_open());

    // Right, Cross: the scene's folder (and only it) is gone; the grid
    // re-lists.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "trip" / "s1"));
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s2"));
    CHECK(std::filesystem::is_directory(fixture.root / "trip"));
    REQUIRE(fixture.scenes->names().size() == 1);
    CHECK(fixture.scenes->names()[0] == "s2");
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Deleted s1");
}

TEST_CASE("ui_scenes keeps the focused scene after returning") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s3", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    REQUIRE(fixture.scenes->focused_scene() == 1);
    CHECK(fixture.scenes->names()[1] == "s2");

    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::AlbumPick);
    REQUIRE(fixture.stack.pop());

    // The pop revealed the screen again: it re-listed the disk and put the focus
    // back on the scene the user was on.
    CHECK(fixture.scenes->focused_scene() == 1);
    CHECK(fixture.scenes->names()[1] == "s2");
}

TEST_CASE("ui_scenes plus opens the photo picker for this folder") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Left));  // the "+" tile
    CHECK(fixture.scenes->focused_scene() == -1);
    CHECK(fixture.scenes->hints()[0].label == "Add photos");
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.photos != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Photos);
    CHECK(fixture.photos->last.a == "trip");
    CHECK(fixture.commands.empty());

    // An empty folder starts on the "+" tile.
    Fixture empty;
    std::filesystem::create_directories(empty.root / "new");
    empty.enter("new");
    CHECK(empty.scenes->grid().cells().size() == 1);
    CHECK(empty.scenes->grid().focus() == 0);
}

TEST_CASE("ui_scenes shows conversions into this folder until they finish") {
    Fixture fixture;
    splat::FakeJobService jobs;
    fixture.services.jobs = &jobs;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    REQUIRE(jobs.enqueue({Path("one.jpg"), Path("two.jpg")}, "sharp", "trip").ok());
    REQUIRE(jobs.enqueue({Path("elsewhere.jpg")}, "sharp", "other").ok());
    jobs.tick(1.0);  // one.jpg at 25 %
    fixture.enter("trip");

    // "+", the two pending photos, then the scene; the other folder's job is
    // not here.
    REQUIRE(fixture.scenes->pending() == 2);
    const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
    REQUIRE(cells.size() == 4);
    CHECK(cells[1].caption == "one");
    CHECK(cells[1].badge == "Converting");
    CHECK(cells[1].progress == doctest::Approx(0.25f));
    CHECK(cells[2].caption == "two");
    CHECK(cells[2].badge == "Queued");
    CHECK(cells[2].progress == 0.0f);
    CHECK(fixture.scenes->focused_scene() == 0);
    CHECK(fixture.scenes->grid().focus() == 3);

    // Progress moves in place, without a re-list.
    jobs.tick(1.0);
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->grid().cells()[1].progress == doctest::Approx(0.5f));
    CHECK(fixture.scenes->focused_scene() == 0);

    // Cross on a pending tile says what it is doing, it does not view.
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.commands.empty());
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back().find("one.jpg") == 0);

    // one.jpg finishes (the fake writes no scene): the folder re-lists and the
    // tile is gone; a failed job says so once.
    jobs.tick(3.0);
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->pending() == 1);
    REQUIRE(jobs.cancel(jobs.snapshot()[1].id).ok());
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->pending() == 0);
    CHECK(fixture.scenes->grid().cells().size() == 2);
}

TEST_CASE("ui_scenes draws one calm placeholder for a scene without a thumbnail") {
    // Every missing thumbnail is the same picture placeholder, whatever the
    // name: no initials, no colour per scene.
    splat::UiCanvas first(192, 112);
    splat::UiCanvas second(192, 112);
    splat::draw_placeholder(first, splat::Rect{0, 0, 192, 112}, 12);
    splat::draw_placeholder(second, splat::Rect{0, 0, 192, 112}, 12);
    CHECK(first.pixels() == second.pixels());
    // The rounded corner is cut; the fill is opaque; the picture glyph is
    // lighter than the fill.
    CHECK(first.pixels()[3] == 0);
    const uint8_t* fill = first.pixels().data() + (10 * 192 + 10) * 4;
    CHECK(fill[3] == 255);
    int lighter = 0;
    for (std::size_t index = 0; index < first.pixels().size(); index += 4) {
        if (first.pixels()[index + 3] == 255 && first.pixels()[index] > fill[0] + 30) ++lighter;
    }
    CHECK(lighter > 50);

    // A scene without thumb.jpg gets no thumbnail in its cell: the grid draws
    // this placeholder for it.
    Fixture fixture;
    write_scene(fixture.root / "trip" / "plain", "sharp", 4, 1.0, false);
    fixture.enter("trip");
    REQUIRE(fixture.scenes->grid().cells().size() == 2);
    const splat::GridCell& cell = fixture.scenes->grid().cells()[1];
    CHECK(cell.icon == splat::GridCell::Icon::None);
    CHECK((cell.thumb == nullptr || cell.thumb->empty()));
}
