// AlbumsScreen, the home screen: the grid order (implicit "Unsorted" first,
// then the folders by name, "New folder" last), the per-tile captions, the
// empty state, Open -> Scenes args, the New folder tile and the Triangle menu
// (Rename, Delete for an empty folder) -> Keyboard args, and the re-list plus
// focus restore when the screen is revealed again. The same screen is a network
// library's home (ScreenArgs::b): the share's folders that hold scenes are albums.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/gallery.h"
#include "gallery/visibility.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/screens/albums.h"
#include "ui/screens/locations.h"
#include "ui/screen_stack.h"
#include "ui/ui_state.h"
#include "vfs/io.h"

using splat::Path;

namespace {

// A scene the gallery accepts: scene.psplat and meta.json. The JPEG bytes are a
// stub and thumb.jpg is removed (console scenes have no thumbnail), so the
// screen must fall back to its placeholder.
void write_scene(const Path& folder, const std::string& backend = "sharp") {
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = backend;
    meta.seconds_taken = 12.5;
    const splat::SplatScene scene = splat_test::make_test_scene(4);
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::Status status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
    std::filesystem::remove(splat::scene_files(folder).thumbnail);
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
    splat_test::TempDir temp{"ui-albums"};
    Path root = temp.path() / "scenes";
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    splat::AlbumsScreen* albums = nullptr;
    CapturingScreen* scenes = nullptr;
    CapturingScreen* keyboard = nullptr;

    Fixture() {
        std::filesystem::create_directories(root);
        services.paths.scenes = root;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        stack.set_context(&context);

        registry.add(splat::ScreenId::Albums, [this] {
            auto screen = std::make_unique<splat::AlbumsScreen>();
            albums = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Scenes, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            scenes = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            keyboard = screen.get();
            return screen;
        });
    }

    void enter() { REQUIRE(stack.push(splat::ScreenId::Albums)); }
};

}  // namespace

TEST_CASE("ui_albums lists the implicit folder first, then the folders by name") {
    Fixture fixture;
    write_scene(fixture.root / "loose-a");
    write_scene(fixture.root / "loose-b");
    write_scene(fixture.root / "trip" / "s1");
    write_scene(fixture.root / "trip" / "s2");
    std::filesystem::create_directories(fixture.root / "empty");
    fixture.enter();

    const std::vector<std::string> names = fixture.albums->names();
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "Unsorted");
    CHECK(names[1] == "empty");
    CHECK(names[2] == "trip");
    CHECK(names[3] == "New folder");

    const std::vector<splat::GridCell>& cells = fixture.albums->grid().cells();
    REQUIRE(cells.size() == 4);
    CHECK(cells[3].icon == splat::GridCell::Icon::Add);
    CHECK_FALSE(cells[3].stack);
    CHECK(cells[0].caption == "Unsorted");
    CHECK(cells[0].sublabel == "2 scenes");
    CHECK(cells[1].sublabel == "0 scenes");
    CHECK(cells[2].sublabel == "2 scenes");
    // A folder with scenes shows its cover (or the placeholder while there is
    // no thumbnail - the console case) with a stack behind it; an empty one is
    // a plain folder icon, no stack.
    for (std::size_t index = 0; index < 3; ++index) CHECK(cells[index].thumb != nullptr);
    CHECK(cells[0].stack);
    CHECK_FALSE(cells[1].stack);
    CHECK(cells[2].stack);
    CHECK(cells[1].icon == splat::GridCell::Icon::Folder);
    CHECK(cells[0].icon == splat::GridCell::Icon::None);
    CHECK(cells[2].icon == splat::GridCell::Icon::None);

    CHECK(fixture.albums->title() == "Scenes");
    // Unsorted (focused) is not a folder: no Triangle prompt.
    std::vector<splat::Hint> hints = fixture.albums->hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "Open");
    CHECK(hints[1].button == "Options");
    CHECK(hints[1].label == "Settings");
    fixture.stack.update(fixture.context, press(splat::Right));
    hints = fixture.albums->hints();
    REQUIRE(hints.size() == 3);
    CHECK(hints[1].button == "Triangle");
    CHECK(hints[1].label == "More");
}

TEST_CASE("ui_albums select opens the album's scenes with its folder name") {
    Fixture fixture;
    write_scene(fixture.root / "loose");
    write_scene(fixture.root / "trip" / "s1");
    fixture.enter();

    // The implicit album is the first tile and travels as the empty folder name.
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.scenes != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);
    CHECK(fixture.scenes->enters == 1);
    CHECK(fixture.scenes->last.a.empty());

    REQUIRE(fixture.stack.pop());
    fixture.stack.update(fixture.context, press(splat::Right));  // Unsorted -> trip
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);
    CHECK(fixture.scenes->last.a == "trip");
}

TEST_CASE("ui_albums empty state and the re-list on return") {
    Fixture fixture;
    fixture.enter();
    CHECK(fixture.albums->empty_state());
    CHECK(std::string(splat::AlbumsScreen::kEmptyText) == "No scenes yet");
    // The grid still offers the loose scenes, where "+" converts the first
    // one, and a new folder.
    REQUIRE(fixture.albums->names().size() == 2);
    CHECK(fixture.albums->names()[0] == "Unsorted");
    CHECK(fixture.albums->names()[1] == "New folder");

    // A conversion finishes while the panel is elsewhere: revealing the screen
    // again re-lists the disk instead of trusting a cache.
    write_scene(fixture.root / "trip" / "s1");
    REQUIRE(fixture.stack.push(splat::ScreenId::Scenes));
    REQUIRE(fixture.stack.pop());
    CHECK_FALSE(fixture.albums->empty_state());
    REQUIRE(fixture.albums->names().size() == 3);
    CHECK(fixture.albums->names()[1] == "trip");
    CHECK(fixture.albums->grid().cells()[1].sublabel == "1 scene");
}

TEST_CASE("ui_albums context menu offers the folder's own actions") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1");
    std::filesystem::create_directories(fixture.root / "empty");
    fixture.enter();
    // Unsorted, empty, trip, New folder.

    // An empty folder: Rename and Delete.
    fixture.stack.update(fixture.context, press(splat::Right));
    CHECK(fixture.albums->hints()[1].button == "Triangle");
    CHECK(fixture.albums->hints()[1].label == "More");
    fixture.stack.update(fixture.context, press(splat::Context));
    REQUIRE(fixture.albums->menu().is_open());
    REQUIRE(fixture.albums->menu().items().size() == 2);
    CHECK(fixture.albums->menu().items()[0] == "Rename");
    CHECK(fixture.albums->menu().items()[1] == "Delete");
    CHECK(fixture.albums->menu().title() == "empty");
    CHECK(fixture.albums->hints()[0].label == "Choose");
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);
    CHECK(fixture.keyboard->last.a == "rename-album");
    CHECK(fixture.keyboard->last.b == "empty");
    REQUIRE(fixture.stack.pop());
    CHECK_FALSE(fixture.albums->menu().is_open());

    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.keyboard->last.a == "delete-album");
    CHECK(fixture.keyboard->last.b == "empty");
    REQUIRE(fixture.stack.pop());

    // A folder with scenes cannot be deleted, so it is not offered.
    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Context));
    REQUIRE(fixture.albums->menu().items().size() == 1);
    CHECK(fixture.albums->menu().items()[0] == "Rename");
    // Circle closes the menu (UiApp asks the screen first) and nothing opens.
    CHECK(fixture.albums->handle_back(fixture.context));
    CHECK_FALSE(fixture.albums->menu().is_open());
    CHECK_FALSE(fixture.albums->handle_back(fixture.context));
    CHECK(fixture.stack.top_id() == splat::ScreenId::Albums);

    // The "New folder" tile asks for a name, with the date as a start.
    fixture.stack.update(fixture.context, press(splat::Right));
    CHECK(fixture.albums->hints()[0].label == "New folder");
    fixture.stack.update(fixture.context, press(splat::Context));  // nothing to edit here
    CHECK_FALSE(fixture.albums->menu().is_open());
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.keyboard->last.a == "new-album");
    const std::string expected = splat::default_album_name(std::time(nullptr), fixture.root);
    REQUIRE(fixture.keyboard->last.b.size() >= 10);
    CHECK(fixture.keyboard->last.b.substr(0, 10) == expected.substr(0, 10));
    REQUIRE(fixture.stack.pop());

    // Unsorted is not a folder: no menu and no Triangle prompt.
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Left));
    REQUIRE(fixture.albums->grid().focus() == 0);
    fixture.stack.update(fixture.context, press(splat::Context));
    CHECK_FALSE(fixture.albums->menu().is_open());
    for (const splat::Hint& hint : fixture.albums->hints()) CHECK(hint.button != "Triangle");
}

TEST_CASE("ui_albums menu input does not move the grid") {
    Fixture fixture;
    write_scene(fixture.root / "loose");
    write_scene(fixture.root / "trip" / "s1");
    fixture.enter();
    fixture.stack.update(fixture.context, press(splat::Right));  // trip
    fixture.stack.update(fixture.context, press(splat::Context));
    REQUIRE(fixture.albums->menu().is_open());
    fixture.stack.update(fixture.context, press(splat::Right));
    CHECK(fixture.albums->grid().focus() == 1);
    CHECK(fixture.toasts.empty());
}

TEST_CASE("ui_albums keeps the focused album after returning") {
    Fixture fixture;
    write_scene(fixture.root / "loose");
    write_scene(fixture.root / "trip" / "s1");
    fixture.enter();

    fixture.stack.update(fixture.context, press(splat::Right));
    REQUIRE(fixture.albums->grid().focus() == 1);
    CHECK(fixture.albums->names()[1] == "trip");

    REQUIRE(fixture.stack.push(splat::ScreenId::Scenes));
    REQUIRE(fixture.stack.pop());
    CHECK(fixture.albums->grid().focus() == 1);
}

// ---- a network library -------------------------------------------------------

namespace {

// A share "smb0" with a library at lib/: two albums with scenes, scenes loose in
// the library folder, and folders that are not scene libraries.
struct ShareFixture : Fixture {
    std::shared_ptr<splat_test::TreeFileSystem> share =
        std::make_shared<splat_test::TreeFileSystem>("smb0");

    ShareFixture() {
        share->files["lib/Trips/s1/scene.psplat"] = {1};
        share->files["lib/Trips/s1/meta.json"] = {2};
        share->files["lib/Trips/s2/scene.psplat"] = {3};
        share->files["lib/Made/m1/scene.psplat"] = {4};
        share->files["lib/loose/scene.psplat"] = {5};
        share->files["lib/Documents/notes.txt"] = {6};      // not a scene library
        share->files["lib/Documents/old/readme.txt"] = {7};
        splat::vfs::file_systems().add(share);
    }
    ~ShareFixture() { splat::vfs::file_systems().remove("smb0"); }

    void enter_library(const std::string& root) {
        splat::ScreenArgs args;
        args.b = root;
        REQUIRE(stack.push(splat::ScreenId::Albums, args));
        pump();
    }

    // A screen revealed again re-lists its share on the worker; frames pass until
    // that listing has been published, then input is read again.
    void settle() {
        for (int frame = 0; frame < 50; ++frame) {
            stack.update(context, splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // The listing runs on a worker and is published by update().
    void pump() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (albums->grid().cells().empty() && albums->error().empty() && !albums->blocked() &&
               std::chrono::steady_clock::now() < deadline) {
            stack.update(context, splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};

}  // namespace

TEST_CASE("ui_albums a network library lists the folders that hold scenes as albums") {
    ShareFixture fixture;
    fixture.enter_library("smb0:/lib");
    REQUIRE(fixture.albums->root() == Path("smb0:/lib"));

    // "Unsorted" holds the scenes directly in the library folder; the albums are
    // the subfolders with scenes; a folder with none (Documents) is not one; there
    // is no "New folder" tile on a share.
    const std::vector<std::string> names = fixture.albums->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Unsorted");
    CHECK(names[1] == "Made");
    CHECK(names[2] == "Trips");
    const std::vector<splat::GridCell>& cells = fixture.albums->grid().cells();
    CHECK(cells[0].sublabel == "1 scene");
    CHECK(cells[1].sublabel == "1 scene");
    CHECK(cells[2].sublabel == "2 scenes");
    CHECK_FALSE(fixture.albums->empty_state());
    CHECK(fixture.albums->error().empty());
}

TEST_CASE("ui_albums opens a share's album with the library root and returns to the same tile") {
    ShareFixture fixture;
    fixture.enter_library("smb0:/lib");

    fixture.stack.update(fixture.context, press(splat::Right));  // Made
    fixture.stack.update(fixture.context, press(splat::Right));  // Trips
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::Scenes);
    CHECK(fixture.scenes->last.a == "Trips");
    CHECK(fixture.scenes->last.b == "smb0:/lib");
    fixture.scenes = nullptr;
    REQUIRE(fixture.stack.pop());  // back to the albums, on the same tile
    fixture.settle();
    CHECK(fixture.albums->grid().focus() == 2);

    // The loose scenes travel as the empty album, with the root.
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.scenes != nullptr);
    CHECK(fixture.scenes->last.a.empty());
    CHECK(fixture.scenes->last.b == "smb0:/lib");
}

TEST_CASE("ui_albums a share's folders cannot be changed from the console") {
    ShareFixture fixture;
    fixture.enter_library("smb0:/lib");
    fixture.stack.update(fixture.context, press(splat::Right));  // Made
    for (const splat::Hint& hint : fixture.albums->hints()) CHECK(hint.button != "Triangle");
    fixture.stack.update(fixture.context, press(splat::Context));
    CHECK_FALSE(fixture.albums->menu().is_open());
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == splat::kRemoteMutationRefusal);
}

TEST_CASE("ui_albums an offline or missing share says so instead of an empty library") {
    ShareFixture fixture;
    fixture.enter_library("smb0:/no-such-folder");
    CHECK(fixture.albums->error().find("no-such-folder") != std::string::npos);
    CHECK(fixture.albums->names().empty());
}

TEST_CASE("ui_albums a library with no scene yet still offers Unsorted") {
    ShareFixture fixture;
    fixture.share->files.clear();
    fixture.share->files["lib/readme.txt"] = {1};
    fixture.enter_library("smb0:/lib");
    REQUIRE(fixture.albums->names().size() == 1);
    CHECK(fixture.albums->names()[0] == "Unsorted");
    CHECK(fixture.albums->empty_state());
}

TEST_CASE("ui_albums a hidden network library is blocked and not even listed") {
    ShareFixture fixture;
    splat::Visibility state = splat::Visibility::all_visible(splat::VisibilityContext{});
    state.enabled = true;
    state.allowed.push_back(splat::VisibilityKey::location("smb1"));  // smb0 unticked
    splat::VisibilityScope scope(std::move(state));

    fixture.share->forget_list_calls();
    fixture.enter_library("smb0:/lib");
    CHECK(fixture.albums->blocked());
    CHECK(fixture.albums->names().empty());
    CHECK(fixture.share->list_calls().empty());
    REQUIRE(fixture.albums->hints().size() == 1);
    CHECK(fixture.albums->hints()[0].label == "Back");
}

TEST_CASE("ui_albums an Output folder inside a location's Path leaves its scenes visible") {
    // The location smb0 has its Path at "scenes". Scenes were there before the
    // Output folder was chosen; the Output folder (scenes/Made) holds the albums
    // new scenes are saved into.
    ShareFixture fixture;
    fixture.share->files.clear();
    fixture.share->files["scenes/old/s1/scene.psplat"] = {1};
    fixture.share->files["scenes/loose/scene.psplat"] = {2};
    fixture.share->files["scenes/Made/Trips/n1/scene.psplat"] = {3};
    fixture.share->files["scenes/Made/Trips/n2/scene.psplat"] = {4};

    splat::FakeLocationService locations;
    splat::UiState state;
    state.output_folder = "smb0:/scenes/Made";
    fixture.services.locations = &locations;
    fixture.services.state = &state;
    fixture.registry.add(splat::ScreenId::Locations,
                         [] { return std::make_unique<splat::LocationsScreen>(); });
    REQUIRE(fixture.stack.push(splat::ScreenId::Locations));
    fixture.stack.update(fixture.context, splat::UiInput{});

    // The location's row opens its Path, exactly as without an Output folder:
    // the existing album "old" and the loose scene are there.
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::Albums);
    fixture.pump();
    CHECK(fixture.albums->root() == Path("smb0:/scenes"));
    REQUIRE(fixture.albums->names().size() == 2);
    CHECK(fixture.albums->names()[0] == "Unsorted");
    CHECK(fixture.albums->names()[1] == "old");
    CHECK(fixture.albums->grid().cells()[0].sublabel == "1 scene");
    REQUIRE(fixture.stack.pop());

    // The Output folder is the next row: the albums new scenes are saved into.
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::Albums);
    fixture.pump();
    CHECK(fixture.albums->root() == Path("smb0:/scenes/Made"));
    REQUIRE(fixture.albums->names().size() == 2);
    CHECK(fixture.albums->names()[0] == "Unsorted");
    CHECK(fixture.albums->names()[1] == "Trips");
    CHECK(fixture.albums->grid().cells()[1].sublabel == "2 scenes");
}
