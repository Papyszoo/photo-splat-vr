// Output folder (0.6.0): where every new scene is saved. The setting in ui.json
// (default, round trip, a foreign or hand-edited value refused), the Settings
// row, the chooser (the photo picker's folder browser in its choose-a-folder
// mode over an in-memory share), the convert dialog and the picker's import
// sending the scene to the chosen folder under the shelf's album, and the
// Locations screen keeping each location's library and listing the Output folder
// beside it.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/path.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/screen_stack.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screens/locations.h"
#include "ui/screens/photos.h"
#include "ui/screens/settings.h"
#include "ui/ui_state.h"
#include "vfs/io.h"

using splat::Path;

namespace {

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

void write_text(const Path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    REQUIRE(static_cast<bool>(out));
}

std::string read_text(const Path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool has_hint(const std::vector<splat::Hint>& hints, const std::string& button,
              const std::string& label) {
    for (const splat::Hint& hint : hints) {
        if (hint.button == button && hint.label == label) return true;
    }
    return false;
}

class StubScreen : public splat::Screen {
public:
    explicit StubScreen(std::string* seen_b = nullptr) : seen_b_(seen_b) {}
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override {
        if (seen_b_ != nullptr) *seen_b_ = args.b;
    }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }

private:
    std::string* seen_b_;
};

// The panel's services over a temp tree, with two network locations: smb0
// ("nas.local/Photos", path "scenes", a photo source) and smb1
// ("192.168.1.20/Scenes", the share root, not a photo source), each backed by an
// in-memory share that lists folders and files.
struct World {
    splat_test::TempDir dir{"ui-output-folder"};
    splat::FakeLocationService locations;
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::UiState state;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::shared_ptr<splat_test::TreeFileSystem> smb0 =
        std::make_shared<splat_test::TreeFileSystem>("smb0");
    std::shared_ptr<splat_test::TreeFileSystem> smb1 =
        std::make_shared<splat_test::TreeFileSystem>("smb1");
    std::vector<std::string> toasts;
    std::string library_root_seen;

    World() {
        smb0->files["scenes/a.jpg"] = {1};
        smb0->files["scenes/2026/b.jpg"] = {2};
        smb0->files["scenes/2026/july/c.jpg"] = {3};
        smb0->files["scenes/Made/old/scene.psplat"] = {4};
        smb0->files["scenes/robot.spz"] = {5};
        smb1->files["Splats/x.jpg"] = {6};
        smb1->files["Other/y.jpg"] = {7};
        splat::vfs::file_systems().add(smb0);
        splat::vfs::file_systems().add(smb1);

        std::error_code error;
        std::filesystem::create_directories(dir.path() / "inbox", error);
        std::filesystem::create_directories(dir.path() / "scenes", error);
        std::filesystem::create_directories(dir.path() / "mnt", error);
        services.locations = &locations;
        services.jobs = &jobs;
        services.models = &models;
        services.state = &state;
        services.paths.inbox = dir.path() / "inbox";
        services.paths.scenes = dir.path() / "scenes";
        services.paths.mnt = dir.path() / "mnt";
        services.paths.data = dir.path() / "data";
        services.paths.state = dir.path() / "state";
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        stack.set_context(&context);
        registry.add(splat::ScreenId::Settings,
                     [] { return std::unique_ptr<splat::Screen>(new splat::SettingsScreen()); });
        registry.add(splat::ScreenId::Photos,
                     [] { return std::unique_ptr<splat::Screen>(new splat::PhotosScreen()); });
        registry.add(splat::ScreenId::Locations,
                     [] { return std::unique_ptr<splat::Screen>(new splat::LocationsScreen()); });
        registry.add(splat::ScreenId::Scenes, [this] {
            return std::unique_ptr<splat::Screen>(new StubScreen(&library_root_seen));
        });
        registry.add(splat::ScreenId::Albums, [this] {
            return std::unique_ptr<splat::Screen>(new StubScreen(&library_root_seen));
        });
        registry.add(splat::ScreenId::ConvertDialog, [this] {
            auto screen = std::make_unique<splat::ConvertDialog>();
            dialog = screen.get();
            return std::unique_ptr<splat::Screen>(std::move(screen));
        });
    }

    ~World() {
        splat::vfs::file_systems().remove("smb0");
        splat::vfs::file_systems().remove("smb1");
    }

    splat::ConvertDialog* dialog = nullptr;

    splat::PhotosScreen* chooser() {
        return dynamic_cast<splat::PhotosScreen*>(stack.top());
    }

    void open_chooser() {
        // Something under the chooser, as in the app: the root screen cannot pop.
        if (stack.empty()) REQUIRE(stack.push(splat::ScreenId::Settings));
        splat::ScreenArgs args;
        args.a = splat::PhotosScreen::kPickFolderMode;
        REQUIRE(stack.push(splat::ScreenId::Photos, args));
        REQUIRE(chooser() != nullptr);
    }

    void update(const splat::UiInput& input) { stack.update(context, input); }

    // Pumps frames until the chooser's grid has `count` tiles (the listing runs
    // on a worker and is published by update()).
    void wait_cells(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (chooser()->grid().cells().size() != count &&
               std::chrono::steady_clock::now() < deadline) {
            update(splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(chooser()->grid().cells().size() == count);
    }

    // Pumps frames until the folder just entered has been listed: tiles, or the
    // empty-folder line.
    void wait_listed() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (chooser()->grid().cells().empty() && chooser()->message().empty() &&
               std::chrono::steady_clock::now() < deadline) {
            update(splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE((!chooser()->grid().cells().empty() || !chooser()->message().empty()));
    }

    void open_tab(const std::string& id) {
        int tab = -1;
        for (std::size_t index = 0; index < chooser()->tabs().size(); ++index) {
            if (chooser()->tabs()[index].id == id) tab = static_cast<int>(index);
        }
        REQUIRE_MESSAGE(tab >= 0, "no chooser tab for " << id);
        chooser()->set_tab(context, tab);
    }

    bool has_toast(const std::string& text) const {
        for (const std::string& toast : toasts) {
            if (toast == text) return true;
        }
        return false;
    }
};

std::string tab_ids(const splat::PhotosScreen& screen) {
    std::string ids;
    for (const splat::PhotoTab& tab : screen.tabs()) ids += (ids.empty() ? "" : ",") + tab.id;
    return ids;
}

}  // namespace

// ---- the setting --------------------------------------------------------------

TEST_CASE("output folder: the console is the default and a folder round-trips through ui.json") {
    splat_test::TempDir dir("output-folder-state");
    CHECK(splat::UiState{}.output_folder.empty());
    CHECK(splat::load_ui_state(dir.path()).output_folder.empty());  // no file at all

    splat::UiState state;
    state.output_folder = "smb0:/scenes/Made";
    state.thumbnail_size = 4;
    REQUIRE(splat::save_ui_state(dir.path(), state).ok());
    const splat::UiState loaded = splat::load_ui_state(dir.path());
    CHECK(loaded.output_folder == "smb0:/scenes/Made");
    CHECK(loaded.thumbnail_size == 4);  // the other settings travel with it

    // Back to the console: the key is written empty and reads back empty.
    state.output_folder.clear();
    REQUIRE(splat::save_ui_state(dir.path(), state).ok());
    CHECK(splat::load_ui_state(dir.path()).output_folder.empty());
    CHECK(nlohmann::json::parse(read_text(splat::ui_state_path(dir.path())))
              .at("output_folder")
              .get<std::string>()
              .empty());

    // A share's top folder is a valid choice.
    state.output_folder = "smb1:/";
    REQUIRE(splat::save_ui_state(dir.path(), state).ok());
    CHECK(splat::load_ui_state(dir.path()).output_folder == "smb1:/");
}

TEST_CASE("output folder: a value the folder browser could not have produced is refused") {
    splat_test::TempDir dir("output-folder-invalid");
    const Path file = splat::ui_state_path(dir.path());
    const auto load = [&](const std::string& json) {
        write_text(file, json);
        return splat::load_ui_state(dir.path());
    };

    const std::vector<std::string> invalid = {
             "/data/ps5-splat-vr/scenes",  // a local path is not a network folder
             "scenes/Made",                // relative
             "smb0",                       // no folder part
             "smb0:",                      // no slash
             "smb0:/a/../b",               // climbs out of the folder
             "smb0:/..",                   //
             "smb0:/./a",                  //
             "smb0:/a//b",                 // not the canonical spelling
             "smb0:/a/",                   //
             "SMB0:/a",                    // not a location id
             "0smb:/a",                    //
             " smb0:/a",                   //
             "smb0:/a\\b",                 // a backslash
             std::string("smb0:/a") + char(1) + "b",  // a control character
             std::string("smb0:/") + std::string(2000, 'x'),  // longer than any folder name path
    };
    for (const std::string& bad : invalid) {
        const splat::UiState state =
            load("{\"thumbnail_size\":4,\"output_folder\":" + nlohmann::json(bad).dump() + "}");
        CHECK_MESSAGE(state.output_folder.empty(), "accepted: " << bad.substr(0, 40));
        // The rest of the file is still honoured: only the field is refused.
        CHECK(state.thumbnail_size == 4);
    }
    // Wrong types.
    for (const char* json : {"{\"output_folder\":5}", "{\"output_folder\":true}",
                             "{\"output_folder\":null}", "{\"output_folder\":[\"smb0:/a\"]}",
                             "{\"output_folder\":{\"smb0\":\"/a\"}}"}) {
        CHECK_MESSAGE(load(json).output_folder.empty(), json);
    }
    // A file that is not a state file at all is the defaults.
    CHECK(load("{ not json").output_folder.empty());
    CHECK(load("[\"smb0:/a\"]").output_folder.empty());
    CHECK(load("").output_folder.empty());
    // And a good one next to them loads.
    CHECK(load("{\"output_folder\":\"smb0:/scenes/Made\"}").output_folder == "smb0:/scenes/Made");

    // Saving never writes what loading would refuse.
    splat::UiState state;
    state.output_folder = "smb0:/a/../b";
    REQUIRE(splat::save_ui_state(dir.path(), state).ok());
    CHECK(nlohmann::json::parse(read_text(file)).at("output_folder").get<std::string>().empty());
}

TEST_CASE("output folder: validation and the canonical spelling of a browsed folder") {
    CHECK(splat::valid_output_folder("smb0:/"));
    CHECK(splat::valid_output_folder("smb0:/scenes"));
    CHECK(splat::valid_output_folder("nfs1:/exports/photos 2026/Made"));
    CHECK_FALSE(splat::valid_output_folder(""));

    CHECK(splat::canonical_output_folder(Path("smb0:/scenes//Made/")) == "smb0:/scenes/Made");
    CHECK(splat::canonical_output_folder(Path("smb0:/")) == "smb0:/");
    CHECK(splat::canonical_output_folder(Path("smb0:/a/../b")).empty());
    CHECK(splat::canonical_output_folder(Path("/data/scenes")).empty());
}

TEST_CASE("output folder: the shelf's own network library wins, else the setting, else the console") {
    World world;
    splat::UiState state;
    const Path local = world.dir.path() / "scenes";

    // No setting: the shelf and its album, as before.
    auto plain = splat::scene_target(&state, &world.locations, local, "Trips");
    REQUIRE(plain.ok());
    CHECK(plain.value().root.empty());
    CHECK(plain.value().album == "Trips");
    CHECK_FALSE(plain.value().from_setting);
    auto no_state = splat::scene_target(nullptr, &world.locations, local, "Trips");
    REQUIRE(no_state.ok());
    CHECK(no_state.value().root.empty());

    // With the setting, a console shelf saves into the folder, in the shelf's
    // album: <output folder>/Trips/<scene>.
    state.output_folder = "smb0:/scenes/Made";
    auto chosen = splat::scene_target(&state, &world.locations, local, "Trips");
    REQUIRE(chosen.ok());
    CHECK(chosen.value().root == Path("smb0:/scenes/Made"));
    CHECK(chosen.value().album == "Trips");
    CHECK(chosen.value().from_setting);
    // No album (Unsorted): the same default the console uses, the loose scenes.
    auto loose = splat::scene_target(&state, &world.locations, local, "");
    REQUIRE(loose.ok());
    CHECK(loose.value().root == Path("smb0:/scenes/Made"));
    CHECK(loose.value().album.empty());
    // A name the console's album rules refuse cannot be made on the share: an
    // error, never a rewritten or dropped album.
    for (const char* bad : {"../x", "a/b", ".hidden", "Caf\xC3\xA9"}) {
        const auto refused = splat::scene_target(&state, &world.locations, local, bad);
        CHECK_MESSAGE(!refused.ok(), bad);
    }

    // A network library the picker was opened for keeps its own destination.
    auto shelf = splat::scene_target(&state, &world.locations, Path("smb1:/Splats"), "");
    REQUIRE(shelf.ok());
    CHECK(shelf.value().root == Path("smb1:/Splats"));
    CHECK_FALSE(shelf.value().from_setting);
    auto shelf_album = splat::scene_target(&state, &world.locations, Path("smb1:/Splats"), "Trips");
    REQUIRE(shelf_album.ok());
    CHECK(shelf_album.value().album == "Trips");

    // A location that was removed is an error: never a scene on the console.
    state.output_folder = "smb9:/scenes";
    const auto gone = splat::scene_target(&state, &world.locations, local, "Trips");
    CHECK_FALSE(gone.ok());
    CHECK(gone.message().find("removed") != std::string::npos);
    CHECK(splat::output_folder_text(&state, &world.locations) == "smb9 (removed)");
}

TEST_CASE("output folder: the row and the destination read as location and folder names") {
    World world;
    splat::UiState state;
    CHECK(splat::output_folder_text(&state, &world.locations) == "Console");
    CHECK(splat::output_folder_text(nullptr, &world.locations) == "Console");
    state.output_folder = "smb0:/scenes/2026/july";
    CHECK(splat::output_folder_text(&state, &world.locations) ==
          "nas.local/Photos \xE2\x80\xBA scenes \xE2\x80\xBA 2026 \xE2\x80\xBA july");
    state.output_folder = "smb1:/";
    CHECK(splat::output_folder_text(&state, &world.locations) == "192.168.1.20/Scenes");
}

// ---- Settings and the chooser --------------------------------------------------

TEST_CASE("output folder: the Settings row shows the choice and opens the chooser") {
    World world;
    REQUIRE(world.stack.push(splat::ScreenId::Settings));
    auto* settings = dynamic_cast<splat::SettingsScreen*>(world.stack.top());
    REQUIRE(settings != nullptr);
    CHECK(settings->list().rows()[splat::SettingsScreen::OutputFolder].label == "Output folder");
    CHECK(settings->list().rows()[splat::SettingsScreen::OutputFolder].right == "Console");
    CHECK_FALSE(settings->list().rows()[splat::SettingsScreen::OutputFolder].dim);

    // The value follows the setting on the next frame.
    world.state.output_folder = "smb0:/scenes/Made";
    world.update(splat::UiInput{});
    CHECK(settings->list().rows()[splat::SettingsScreen::OutputFolder].right ==
          "nas.local/Photos \xE2\x80\xBA scenes \xE2\x80\xBA Made");

    while (settings->list().focus() != splat::SettingsScreen::OutputFolder) {
        world.update(press(splat::Down));
    }
    world.update(press(splat::Select));
    REQUIRE(world.stack.top_id() == splat::ScreenId::Photos);
    REQUIRE(world.chooser() != nullptr);
    CHECK(world.chooser()->picking_folder());
    CHECK(world.chooser()->title() == "Output folder");
}

TEST_CASE("output folder: the chooser offers the console and every location, folders only") {
    World world;
    world.open_chooser();
    // Console first, then every configured location - smb1 is not a photo source
    // and is here anyway. No Inbox, USB, Captures or "add" tab.
    CHECK(tab_ids(*world.chooser()) == "console-output,smb0,smb1");
    CHECK(world.chooser()->tabs()[0].title == "Console");
    CHECK(world.chooser()->message() == "New scenes are saved on this console.");
    CHECK(has_hint(world.chooser()->hints(), "Square", "Use the console"));
    CHECK(has_hint(world.chooser()->hints(), "Circle", "Back"));

    world.open_tab("smb0");
    world.wait_cells(2);  // 2026/ and Made/; a.jpg and robot.spz are files, not listed
    const std::vector<splat::GridCell>& cells = world.chooser()->grid().cells();
    CHECK(cells[0].caption == "2026");
    CHECK(cells[1].caption == "Made");
    for (const splat::GridCell& cell : cells) CHECK(cell.icon == splat::GridCell::Icon::Folder);
}

TEST_CASE("output folder: Cross opens a folder, Circle goes up, Square uses the folder shown") {
    World world;
    world.open_chooser();
    world.open_tab("smb0");
    world.wait_cells(2);
    CHECK(world.chooser()->location() == "Top folder of nas.local/Photos");
    CHECK(has_hint(world.chooser()->hints(), "Cross", "Open"));
    CHECK(has_hint(world.chooser()->hints(), "Square", "Use this folder"));

    world.update(press(splat::Select));  // 2026/
    world.wait_cells(1);                  // july/
    CHECK(world.chooser()->browse_dir() == Path("smb0:/scenes/2026"));
    CHECK(world.chooser()->location() == "2026");
    world.update(press(splat::Select));  // july/
    world.wait_listed();                 // nothing below it
    CHECK(world.chooser()->message() == "No folders here");
    CHECK(world.chooser()->location() == "2026 \xE2\x80\xBA july");
    // An empty folder can still be chosen.
    CHECK(has_hint(world.chooser()->hints(), "Square", "Use this folder"));

    // Circle climbs one level at a time; the chooser keeps the screen.
    CHECK(world.chooser()->handle_back(world.context));
    CHECK(world.chooser()->browse_dir() == Path("smb0:/scenes/2026"));
    world.wait_cells(1);

    const std::size_t depth = world.stack.size();
    world.update(press(splat::Secondary));
    // The choice is stored, saved to ui.json, and the chooser closes.
    CHECK(world.state.output_folder == "smb0:/scenes/2026");
    CHECK(splat::load_ui_state(world.services.paths.state).output_folder == "smb0:/scenes/2026");
    CHECK(world.stack.size() == depth - 1);
    CHECK(world.has_toast("New scenes are saved to nas.local/Photos \xE2\x80\xBA scenes "
                          "\xE2\x80\xBA 2026"));
    // The share's listings were one folder at a time.
    CHECK(world.smb0->list_calls() ==
          std::vector<std::string>{"scenes", "scenes/2026", "scenes/2026/july", "scenes/2026",
                                   });
}

TEST_CASE("output folder: Square at a location's top uses its own folder, and the console resets it") {
    World world;
    world.open_chooser();
    world.open_tab("smb0");
    world.wait_cells(2);
    world.update(press(splat::Secondary));
    CHECK(world.state.output_folder == "smb0:/scenes");  // the location's path
    CHECK(splat::load_ui_state(world.services.paths.state).output_folder == "smb0:/scenes");

    // The chooser opens where the choice is, on that location's tab and folder.
    world.state.output_folder = "smb0:/scenes/Made";
    world.open_chooser();
    CHECK(world.chooser()->tabs()[static_cast<std::size_t>(world.chooser()->tab())].id == "smb0");
    CHECK(world.chooser()->browse_dir() == Path("smb0:/scenes/Made"));
    world.wait_cells(1);  // old/

    // The Console tab resets the setting.
    world.open_tab("console-output");
    CHECK(world.chooser()->message() == "New scenes are saved on this console.");
    world.update(press(splat::Secondary));
    CHECK(world.state.output_folder.empty());
    CHECK(splat::load_ui_state(world.services.paths.state).output_folder.empty());
    CHECK(world.has_toast("New scenes are saved on this console"));
}

TEST_CASE("output folder: choosing a folder switches the location's scene library on") {
    World world;
    splat::LocationView smb1;
    for (const splat::LocationView& view : world.locations.list()) {
        if (view.id == "smb1") smb1 = view;
    }
    smb1.splat_library = false;  // a share added only as a photo source
    REQUIRE(world.locations.update(smb1).ok());

    world.open_chooser();
    world.open_tab("smb1");
    world.wait_cells(2);  // Other/, Splats/
    world.update(press(splat::Select));  // Other/
    world.wait_listed();
    CHECK(world.chooser()->handle_back(world.context));
    world.wait_cells(2);
    world.update(press(splat::Right));   // Splats/
    world.update(press(splat::Select));
    world.wait_listed();
    world.update(press(splat::Secondary));
    CHECK(world.state.output_folder == "smb1:/Splats");
    for (const splat::LocationView& view : world.locations.list()) {
        if (view.id == "smb1") CHECK(view.splat_library);
    }
}

TEST_CASE("output folder: a save that fails keeps the old choice and says so") {
    World world;
    // The state folder is "inside a file": it cannot be created.
    write_text(world.dir.path() / "blocker", "x");
    world.services.paths.state = world.dir.path() / "blocker" / "state";
    world.state.output_folder = "smb0:/scenes";
    world.open_chooser();
    world.open_tab("smb1");
    world.wait_cells(2);
    const std::size_t depth = world.stack.size();
    world.update(press(splat::Secondary));
    CHECK(world.state.output_folder == "smb0:/scenes");
    CHECK(world.has_toast("Could not save settings"));
    CHECK(world.stack.size() == depth);  // still choosing
}

// ---- the conversion goes to the chosen folder -----------------------------------

TEST_CASE("output folder: the convert dialog queues into the chosen folder, in the shelf's album") {
    World world;
    world.state.output_folder = "smb0:/scenes/Made";
    splat::ScreenArgs args;
    args.a = "Trips";
    args.b = world.services.paths.scenes.string();  // the console's own shelf
    args.list.push_back((world.dir.path() / "inbox" / "a.jpg").string());
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    REQUIRE(world.dialog != nullptr);

    CHECK(world.dialog->output_root() == Path("smb0:/scenes/Made"));
    CHECK(world.dialog->album() == "Trips");
    CHECK(world.dialog->folder_line() ==
          "into nas.local/Photos \xE2\x80\xBA scenes \xE2\x80\xBA Made \xE2\x80\xBA Trips");
    CHECK(world.dialog->message().empty());

    world.update(press(splat::Select));
    REQUIRE(world.jobs.snapshot().size() == 1);
    CHECK(world.jobs.last_output_root() == Path("smb0:/scenes/Made"));
    CHECK(world.jobs.snapshot()[0].album == "Trips");
    CHECK(world.jobs.snapshot()[0].output_root == Path("smb0:/scenes/Made"));
}

TEST_CASE("output folder: Unsorted converts loose into the folder, a bad album name is refused") {
    World world;
    world.state.output_folder = "smb0:/scenes/Made";
    splat::ScreenArgs args;
    args.a = "";  // the Unsorted shelf
    args.b = world.services.paths.scenes.string();
    args.list.push_back((world.dir.path() / "inbox" / "a.jpg").string());
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    CHECK(world.dialog->album().empty());
    CHECK(world.dialog->folder_line() == "into nas.local/Photos \xE2\x80\xBA scenes \xE2\x80\xBA Made");
    world.update(press(splat::Select));
    REQUIRE(world.jobs.snapshot().size() == 1);
    CHECK(world.jobs.snapshot()[0].album.empty());
    CHECK(world.jobs.snapshot()[0].output_root == Path("smb0:/scenes/Made"));

    // A console folder whose name the album rules refuse stops the queue with a
    // reason; nothing is queued, nothing goes to the console.
    args.a = "Caf\xC3\xA9";
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    CHECK(world.dialog->message().find("cannot be used on a network share") != std::string::npos);
    world.update(press(splat::Select));
    CHECK(world.jobs.snapshot().size() == 1);
}

TEST_CASE("output folder: without the setting the dialog keeps the album and the console") {
    World world;
    splat::ScreenArgs args;
    args.a = "Trips";
    args.b = world.services.paths.scenes.string();
    args.list.push_back((world.dir.path() / "inbox" / "a.jpg").string());
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    CHECK(world.dialog->output_root().empty());
    CHECK(world.dialog->album() == "Trips");
    CHECK(world.dialog->folder_line() == "into Trips");
    world.update(press(splat::Select));
    REQUIRE(world.jobs.snapshot().size() == 1);
    CHECK(world.jobs.last_output_root().empty());
    CHECK(world.jobs.snapshot()[0].album == "Trips");
}

TEST_CASE("output folder: a network library the picker was opened for is not overridden") {
    World world;
    world.state.output_folder = "smb0:/scenes/Made";
    splat::ScreenArgs args;
    args.a = "";
    args.b = "smb1:/Splats";  // the user browsed another share's library and pressed +
    args.list.push_back((world.dir.path() / "inbox" / "a.jpg").string());
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    CHECK(world.dialog->output_root() == Path("smb1:/Splats"));
    world.update(press(splat::Select));
    CHECK(world.jobs.last_output_root() == Path("smb1:/Splats"));
}

TEST_CASE("output folder: a removed location stops the conversion instead of using the console") {
    World world;
    world.state.output_folder = "smb9:/scenes";
    splat::ScreenArgs args;
    args.a = "Trips";
    args.b = world.services.paths.scenes.string();
    args.list.push_back((world.dir.path() / "inbox" / "a.jpg").string());
    REQUIRE(world.stack.push(splat::ScreenId::ConvertDialog, args));
    CHECK(world.dialog->message().find("removed") != std::string::npos);
    world.update(press(splat::Select));
    CHECK(world.jobs.snapshot().empty());
    REQUIRE_FALSE(world.toasts.empty());
    CHECK(world.toasts.back().find("removed") != std::string::npos);
}

TEST_CASE("output folder: importing a splat file from the console goes to the chosen folder, in its album") {
    World world;
    world.state.output_folder = "smb1:/Splats";
    write_text(world.dir.path() / "data" / "robot.spz", "not a real splat, never parsed here");
    splat::ScreenArgs args;
    args.a = "Trips";
    REQUIRE(world.stack.push(splat::ScreenId::Photos, args));
    auto* picker = world.chooser();
    REQUIRE(picker != nullptr);
    int tab = -1;
    for (std::size_t index = 0; index < picker->tabs().size(); ++index) {
        if (picker->tabs()[index].console) tab = static_cast<int>(index);
    }
    REQUIRE(tab >= 0);
    picker->set_tab(world.context, tab);
    world.update(splat::UiInput{});
    REQUIRE(picker->grid().cells().size() == 1);
    CHECK(picker->grid().cells()[0].badge == "SPLAT");

    world.update(press(splat::Select));
    REQUIRE(world.jobs.last_import().size() == 1);
    CHECK(world.jobs.last_import()[0] == world.dir.path() / "data" / "robot.spz");
    CHECK(world.jobs.last_output_root() == Path("smb1:/Splats"));
    // The picker was opened for the album "Trips": <output folder>/Trips/<scene>.
    CHECK(world.jobs.last_import_album() == "Trips");
}

// ---- the library of the output location ---------------------------------------

TEST_CASE("output folder: a location's library never changes, the Output folder is a row beside it") {
    World world;
    world.stack.set_context(&world.context);
    REQUIRE(world.stack.push(splat::ScreenId::Locations));
    world.update(splat::UiInput{});
    auto* locations = dynamic_cast<splat::LocationsScreen*>(world.stack.top());
    REQUIRE(locations != nullptr);

    // No setting: the location's own folder, as before, as the albums home.
    REQUIRE(locations->names().size() == 3);
    world.update(press(splat::Select));  // the first row, smb0
    REQUIRE(world.stack.top_id() == splat::ScreenId::Albums);
    CHECK(world.library_root_seen == "smb0:/scenes");
    world.stack.pop();

    // The Output folder is inside smb0's Path ("scenes"): the new scenes lie one
    // level deeper than the library lists, so the Output folder is a row of its
    // own and smb0's row still opens its Path with every scene it had.
    world.state.output_folder = "smb0:/scenes/Made";
    world.update(splat::UiInput{});
    REQUIRE(locations->names().size() == 4);
    CHECK(locations->list().rows()[0].label == "nas.local/Photos");
    CHECK(locations->list().rows()[1].label ==
          "nas.local/Photos \xE2\x80\xBA scenes \xE2\x80\xBA Made");
    CHECK(locations->list().rows()[1].sublabel.find("Output folder") != std::string::npos);
    locations->set_focus(0);
    world.update(press(splat::Select));
    REQUIRE(world.stack.top_id() == splat::ScreenId::Albums);
    CHECK(world.library_root_seen == "smb0:/scenes");
    world.stack.pop();
    locations->set_focus(1);
    world.update(press(splat::Select));
    REQUIRE(world.stack.top_id() == splat::ScreenId::Albums);
    CHECK(world.library_root_seen == "smb0:/scenes/Made");
    world.stack.pop();

    // Another location is not affected.
    locations->set_focus(2);
    world.update(press(splat::Select));
    REQUIRE(world.stack.top_id() == splat::ScreenId::Albums);
    CHECK(world.library_root_seen == "smb1:/");
    world.stack.pop();

    // An Output folder outside the Path is kept visible the same way.
    world.state.output_folder = "smb0:/elsewhere";
    world.update(splat::UiInput{});
    REQUIRE(locations->names().size() == 4);
    locations->set_focus(1);
    world.update(press(splat::Select));
    CHECK(world.library_root_seen == "smb0:/elsewhere");
    world.stack.pop();

    // The Output folder that is the library itself needs no second row, and the
    // Console setting never has one.
    world.state.output_folder = "smb0:/scenes";
    world.update(splat::UiInput{});
    CHECK(locations->names().size() == 3);
    world.state.output_folder.clear();
    world.update(splat::UiInput{});
    CHECK(locations->names().size() == 3);

    // Edit and delete on the Output folder's row act on its location.
    world.state.output_folder = "smb0:/scenes/Made";
    world.update(splat::UiInput{});
    REQUIRE(locations->names().size() == 4);
    locations->set_focus(1);
    world.update(press(splat::Secondary));
    CHECK(locations->confirm().is_open());
}
