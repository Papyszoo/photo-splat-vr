// The Visible folders screen and its Settings row: the four stages, the masked
// keyboard flow (set, unlock, change, cancel), the immediate tick/save, the
// HideScene command, the lock on leaving and the damaged state file.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/visibility.h"
#include "test_util.h"
#include "ui/app.h"
#include "ui/fakes.h"
#include "ui/screens/registry.h"
#include "ui/screens/settings.h"
#include "ui/screens/visible_folders.h"
#include "ui/screen_stack.h"

namespace {

using splat::Path;
using splat::Visibility;
using splat::VisibilityKey;
using splat::VisibleFoldersScreen;

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

// Captures the arguments of the screen it replaces, so the test can play the
// keyboard's answer without driving the panel grid.
class StubScreen : public splat::Screen {
public:
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override { last = args; }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return last.title; }

    splat::ScreenArgs last;
};

struct Fixture {
    splat_test::TempDir dir{"ui-vis"};
    Path base = dir.path();
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::FakeCaptureService captures{base / "captures"};
    splat::FakeLocationService locations;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    std::vector<splat::UiCommand> commands;
    std::vector<std::string> log;
    StubScreen* keyboard = nullptr;
    VisibleFoldersScreen* screen = nullptr;
    std::unique_ptr<splat::VisibilityScope> scope;

    Fixture() {
        services.paths.scenes = base / "data" / "ps5-splat-vr" / "scenes";
        services.paths.inbox = base / "data" / "ps5-splat-vr" / "inbox";
        services.paths.state = base / "data" / "ps5-splat-vr" / "state";
        services.paths.mnt = base / "mnt";
        services.paths.data = base / "data";
        services.jobs = &jobs;
        services.models = &models;
        services.captures = &captures;
        services.locations = &locations;
        services.log = [this](const std::string& text) { log.push_back(text); };
        std::error_code error;
        std::filesystem::create_directories(services.paths.state, error);
        std::filesystem::create_directories(services.paths.inbox, error);
        std::filesystem::create_directories(services.paths.scenes / "Family A", error);
        std::filesystem::create_directories(services.paths.scenes / "Family B", error);
        std::filesystem::create_directories(services.paths.data / "Photos", error);
        std::filesystem::create_directories(services.paths.mnt / "usb0", error);
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        context.command_sink = [this](splat::UiCommand command) {
            commands.push_back(std::move(command));
        };
        registry.add(splat::ScreenId::VisibleFolders,
                     [] { return std::make_unique<VisibleFoldersScreen>(); });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto stub = std::make_unique<StubScreen>();
            keyboard = stub.get();
            return stub;
        });
        registry.add(splat::ScreenId::Albums, [] { return std::make_unique<StubScreen>(); });
        // on_enter() only runs through a stack that has the context.
        stack.set_context(&context);
        install(Visibility::all_visible(visibility_context()));
    }

    splat::VisibilityContext visibility_context() const {
        splat::VisibilityContext out;
        out.scenes_root = services.paths.scenes;
        out.inbox = services.paths.inbox;
        out.mnt = services.paths.mnt;
        out.data = services.paths.data;
        out.app_data = services.paths.inbox.parent_path();
        return out;
    }

    void install(Visibility state) {
        // The old scope must be gone first: its destructor restores the policy
        // that was there before it, which would undo the new install.
        scope.reset();
        scope = std::make_unique<splat::VisibilityScope>(std::move(state));
    }

    void open() {
        if (stack.empty()) stack.push(splat::ScreenId::Albums);
        stack.push(splat::ScreenId::VisibleFolders);
        screen = static_cast<VisibleFoldersScreen*>(stack.top());
        keyboard = nullptr;
        REQUIRE(screen != nullptr);
    }

    // The keyboard's answer: the stub captured the arguments, the test supplies
    // the text and pops, which reveals (and re-enters) the screen under test.
    void answer(const std::string& text) {
        REQUIRE(keyboard != nullptr);
        // The pop destroys the stub, so it is not read after it; a re-opened
        // keyboard sets the pointer again from its factory.
        StubScreen* current = keyboard;
        keyboard = nullptr;
        REQUIRE(current->last.a == "free-text");
        if (current->last.on_text) current->last.on_text(text);
        stack.pop();
    }

    void cancel_keyboard() {
        REQUIRE(keyboard != nullptr);
        keyboard = nullptr;
        stack.pop();
    }

    int row_named(const std::string& name) const {
        const std::vector<std::string> names = screen->names();
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (names[index] == name) return static_cast<int>(index);
        }
        return -1;
    }

    bool has_log(const std::string& line) const {
        for (const std::string& entry : log) {
            if (entry == line) return true;
        }
        return false;
    }

    bool has_toast(const std::string& text) const {
        for (const std::string& entry : toasts) {
            if (entry == text) return true;
        }
        return false;
    }

    bool has_key(const std::string& text) const {
        for (const VisibilityKey& key : splat::current_visibility()->allowed) {
            if (key.text() == text) return true;
        }
        return false;
    }

    Path state_file() const { return splat::visible_folders_path(services.paths.state); }
};

}  // namespace

TEST_CASE("ui_visibility the settings row appears after locations and says Off or Protected") {
    Fixture fixture;
    splat::SettingsScreen settings;
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    const std::vector<splat::ListRow>& rows = settings.list().rows();
    CHECK(rows[splat::SettingsScreen::Locations].label == "Network locations");
    CHECK(rows[splat::SettingsScreen::VisibleFolders].label == "Visible folders");
    CHECK(rows[splat::SettingsScreen::VisibleFolders].sublabel ==
          "Choose which folders a visitor can see");
    CHECK(rows[splat::SettingsScreen::VisibleFolders].right == "Off");
    CHECK(rows[splat::SettingsScreen::Quit].label == "Quit");
    // The row sits between Locations and Quit, and never names a folder.
    const std::string right = rows[splat::SettingsScreen::VisibleFolders].right;
    CHECK(right.find("Family") == std::string::npos);

    Visibility protected_state = Visibility::all_visible(fixture.visibility_context());
    protected_state.enabled = true;
    protected_state.allowed = {VisibilityKey::album("Family A")};
    splat::set_visibility(protected_state);
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(settings.list().rows()[splat::SettingsScreen::VisibleFolders].right == "Protected");
    // Still no count and no album name.
    CHECK(settings.list().rows()[splat::SettingsScreen::VisibleFolders].sublabel ==
          "Choose which folders a visitor can see");
}

TEST_CASE("ui_visibility opens on the intro without a passcode") {
    Fixture fixture;
    fixture.open();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK_FALSE(fixture.screen->unlocked());
    REQUIRE(fixture.screen->names().size() == 1);
    CHECK(fixture.screen->names()[0] == "Set a passcode");
    const std::vector<splat::Hint> hints = fixture.screen->hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].label == "Set a passcode");
}

TEST_CASE("ui_visibility sets a passcode and starts with nothing ticked") {
    Fixture fixture;
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    REQUIRE(fixture.keyboard != nullptr);
    // The masked free-text keyboard, titled, with the 4-character minimum.
    CHECK(fixture.keyboard->last.a == "free-text");
    CHECK(fixture.keyboard->last.masked);
    CHECK(fixture.keyboard->last.max_length == 32);
    CHECK(fixture.keyboard->last.min_length == 4);
    CHECK(fixture.keyboard->last.title == "Set a passcode");
    CHECK(fixture.keyboard->last.b.empty());

    fixture.answer("2468");
    // The second entry repeats the first.
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.keyboard->last.title == "Repeat the passcode");
    fixture.answer("2468");

    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);
    CHECK(fixture.screen->unlocked());
    CHECK(std::filesystem::exists(fixture.state_file()));
    CHECK(splat::current_visibility()->enabled);
    CHECK(splat::current_visibility()->allowed.empty());
    CHECK(splat::current_visibility()->check_passcode("2468"));
    CHECK(fixture.has_toast("Passcode set. Tick the folders that may be seen."));
    CHECK(fixture.has_log("ui: visible folders: passcode set"));
    // The passcode itself never reaches the log or a toast.
    for (const std::string& line : fixture.log) CHECK(line.find("2468") == std::string::npos);
    for (const std::string& toast : fixture.toasts) CHECK(toast.find("2468") == std::string::npos);
    // The tick list is the scene library only: hidden albums, scene locations
    // and "Other scenes" included; no photo folder has a row any more.
    CHECK(fixture.row_named("Family A") >= 0);
    CHECK(fixture.row_named("Family B") >= 0);
    CHECK(fixture.row_named("Unsorted") >= 0);
    CHECK(fixture.row_named("Other scenes") >= 0);
    CHECK(fixture.row_named("nas.local/Photos") >= 0);
    CHECK(fixture.row_named("192.168.1.20/Scenes") >= 0);
    CHECK(fixture.row_named("Inbox") < 0);
    CHECK(fixture.row_named("USB 0") < 0);
    CHECK(fixture.row_named("Captures") < 0);
    CHECK(fixture.row_named("Photos") < 0);
    CHECK(fixture.row_named("Network locations...") < 0);
    // Nothing is ticked yet, and a save closed the loaded scene.
    CHECK(fixture.commands.size() == 1);
    CHECK(fixture.commands[0].kind == splat::UiCommand::HideScene);
}

TEST_CASE("ui_visibility the list is scene folders, scene locations and the two actions") {
    Fixture fixture;
    // A location that is only a photo source has no row: photos are not hidden
    // by this list any more (stage U1).
    splat::LocationView photos_only;
    photos_only.name = "photos.local/Pictures";
    photos_only.protocol = "smb";
    photos_only.host = "photos.local";
    photos_only.share = "Pictures";
    photos_only.photo_source = true;
    photos_only.splat_library = false;
    std::string photos_id;
    REQUIRE(fixture.locations.add(photos_only, &photos_id).ok());

    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");

    // Headers included, in draw order. No Inbox, USB, Captures, console
    // storage, "Photos" group or "Network locations..." action.
    CHECK(fixture.screen->names() ==
          (std::vector<std::string>{"Scene folders", "Unsorted", "Family A", "Family B",
                                    "Network", "nas.local/Photos", "192.168.1.20/Scenes",
                                    "Other", "Other scenes", "Change passcode",
                                    "Turn off passcode"}));
    // The headers say scene folders, and keep the "not encryption" sentence.
    const std::vector<splat::Hint> hints = fixture.screen->hints();
    CHECK_FALSE(hints.empty());
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);

    // The photos-only location is unreachable from this list: no row, no key.
    for (std::size_t index = 0; index < fixture.screen->names().size(); ++index) {
        const VisibilityKey* key = fixture.screen->key_at(static_cast<int>(index));
        if (key == nullptr) continue;
        CHECK(key->text() != "location:" + photos_id);
    }
}

TEST_CASE("ui_visibility a mismatched or too-short passcode writes nothing") {
    Fixture fixture;
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    fixture.answer("1357");
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK_FALSE(std::filesystem::exists(fixture.state_file()));
    CHECK(fixture.has_toast("The passcodes do not match"));

    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("12");
    fixture.answer("12");
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK_FALSE(std::filesystem::exists(fixture.state_file()));
    CHECK(fixture.has_toast("Passcode must be at least 4 characters"));
}

TEST_CASE("ui_visibility a cancelled keyboard returns to the stage it came from") {
    Fixture fixture;
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.cancel_keyboard();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK(fixture.keyboard == nullptr);  // the keyboard was not re-opened
    CHECK_FALSE(std::filesystem::exists(fixture.state_file()));

    // Cancelling the Repeat entry of a Set returns to the intro, not to the
    // list: no passcode was ever written.
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    REQUIRE(fixture.keyboard != nullptr);
    fixture.cancel_keyboard();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK_FALSE(fixture.screen->unlocked());
    CHECK_FALSE(std::filesystem::exists(fixture.state_file()));
    CHECK_FALSE(splat::current_visibility()->enabled);
}

TEST_CASE("ui_visibility a cancelled unlock keeps the list locked") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    REQUIRE(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);

    // Circle on the masked keyboard is the same as never having typed a code:
    // no list, no tick row, nothing to save.
    fixture.screen->update(fixture.context, press(splat::Select));
    REQUIRE(fixture.keyboard != nullptr);
    fixture.cancel_keyboard();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    CHECK_FALSE(fixture.screen->unlocked());
    REQUIRE(fixture.screen->names().size() == 1);
    CHECK(fixture.screen->names()[0] == "Enter passcode");
    for (const std::string& name : fixture.screen->names()) CHECK(name != "Family B");
    CHECK(splat::current_visibility()->allows_album("Family A"));
    CHECK_FALSE(splat::current_visibility()->allows_album("Family B"));
    CHECK(fixture.commands.empty());  // no save, no HideScene
}

TEST_CASE("ui_visibility a failed save keeps the row as the policy has it") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    REQUIRE(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);
    fixture.commands.clear();

    // The state folder cannot be written: its parent is a regular file, so the
    // save fails before any rename (a read-only card, a full disk).
    {
        std::ofstream blocked(fixture.base / "blocked", std::ios::binary | std::ios::trunc);
        blocked << "not a directory";
    }
    fixture.services.paths.state = fixture.base / "blocked" / "state";

    const int row = fixture.row_named("Family B");
    REQUIRE(row >= 0);
    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.has_toast("Could not save the folder list"));
    // The tick is not kept: the row, the draft and the policy all still hide it.
    CHECK(fixture.screen->list().rows()[static_cast<std::size_t>(row)].right == "Hidden");
    CHECK_FALSE(fixture.screen->draft().allows_album("Family B"));
    CHECK_FALSE(splat::current_visibility()->allows_album("Family B"));
    CHECK(splat::current_visibility()->allows_album("Family A"));
    CHECK(fixture.commands.empty());  // no HideScene for a save that did not happen
}

TEST_CASE("ui_visibility unlock asks every time and says Wrong passcode") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    REQUIRE(fixture.screen->names().size() == 1);
    CHECK(fixture.screen->names()[0] == "Enter passcode");

    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.keyboard->last.title == "Enter passcode");
    fixture.answer("0000");
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    CHECK(fixture.has_toast("Wrong passcode"));
    CHECK(fixture.has_log("ui: visible folders: wrong passcode"));
    CHECK_FALSE(fixture.screen->unlocked());

    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);
    CHECK(fixture.screen->unlocked());
    // "ui: visible folders: passcode on; 1 of N ticked" - counts only.
    bool counted = false;
    for (const std::string& line : fixture.log) {
        if (line.rfind("ui: visible folders: passcode on; 1 of ", 0) == 0) counted = true;
    }
    CHECK(counted);
}

TEST_CASE("ui_visibility the tick count is rows, not inert keys from an older file") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    // The upgrade path: a stage-H file still holds photo keys ("source:inbox",
    // "folder:<abs path>"). They load, they have no row, and they hide nothing.
    state.allowed = {VisibilityKey::album("Family A"), VisibilityKey::photo_source("inbox"),
                     VisibilityKey::console_folder(Path("/data"))};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    REQUIRE(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    REQUIRE(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);

    int rows = 0;
    for (int row = 0; row < static_cast<int>(fixture.screen->list().rows().size()); ++row) {
        if (fixture.screen->key_at(row) != nullptr) ++rows;
    }
    REQUIRE(rows > 0);
    std::string counted;
    for (const std::string& line : fixture.log) {
        if (line.rfind("ui: visible folders: passcode on; ", 0) == 0) counted = line;
    }
    REQUIRE_FALSE(counted.empty());
    int ticked = -1;
    int known = -1;
    REQUIRE(std::sscanf(counted.c_str(), "ui: visible folders: passcode on; %d of %d ticked",
                        &ticked, &known) == 2);
    // One real tick. Counting the state vector (three keys) reported "3 of N"
    // for a list that can show one, and with enough stale keys "N of M" with
    // N > M; the console check greps this line.
    CHECK(ticked == 1);
    CHECK(known == rows);
    CHECK_FALSE(fixture.has_log("ui: visible folders: passcode on; " +
                               std::to_string(state.allowed.size()) + " of " +
                               std::to_string(rows) + " ticked"));
}

TEST_CASE("ui_visibility a tick saves at once and emits HideScene") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    REQUIRE(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);
    fixture.commands.clear();

    const int row = fixture.row_named("Family A");
    REQUIRE(row >= 0);
    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.has_key("album:Family A"));
    CHECK(fixture.screen->list().rows()[static_cast<std::size_t>(row)].right == "Visible");
    // Written immediately: a relaunch reads it back.
    const Visibility reloaded =
        splat::load_visibility(fixture.services.paths.state, fixture.visibility_context()).value();
    CHECK(reloaded.allows_album("Family A"));
    REQUIRE(fixture.commands.size() == 1);
    CHECK(fixture.commands[0].kind == splat::UiCommand::HideScene);

    // Cross again unticks it, and the row says Hidden.
    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK_FALSE(fixture.has_key("album:Family A"));
    CHECK(fixture.screen->list().rows()[static_cast<std::size_t>(row)].right == "Hidden");
    CHECK(fixture.commands.size() == 2);
}

TEST_CASE("ui_visibility a new album appears unticked") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");

    // A folder that appears after the allow-list was saved is listed and hidden
    // until the owner ticks it.
    std::error_code error;
    std::filesystem::create_directories(fixture.services.paths.scenes / "Family C", error);
    fixture.screen->on_enter(fixture.context, splat::ScreenArgs{});  // re-list
    const int row = fixture.row_named("Family C");
    REQUIRE(row >= 0);
    CHECK(fixture.screen->list().rows()[static_cast<std::size_t>(row)].right == "Hidden");
    CHECK_FALSE(splat::current_visibility()->allows_album("Family C"));
    CHECK(splat::current_visibility()->allows_album("Family A"));
}

TEST_CASE("ui_visibility change needs the current passcode and keeps the list") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");

    const int row = fixture.row_named("Change passcode");
    REQUIRE(row >= 0);
    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.keyboard->last.title == "Current passcode");
    // A wrong current passcode changes nothing and stays on the list.
    fixture.answer("0000");
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::list);
    CHECK(fixture.screen->unlocked());
    CHECK(fixture.has_toast("Wrong passcode"));
    CHECK(fixture.has_key("album:Family A"));

    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    CHECK(fixture.keyboard->last.title == "New passcode");
    fixture.answer("1357");
    CHECK(fixture.keyboard->last.title == "Repeat the new passcode");
    fixture.answer("1357");
    CHECK(fixture.has_toast("Passcode changed"));
    CHECK(fixture.has_log("ui: visible folders: passcode changed"));
    CHECK(splat::current_visibility()->check_passcode("1357"));
    CHECK_FALSE(splat::current_visibility()->check_passcode("2468"));
    CHECK(splat::current_visibility()->allows_album("Family A"));
}

TEST_CASE("ui_visibility turn off needs the card and makes every folder visible") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");

    const int row = fixture.row_named("Turn off passcode");
    REQUIRE(row >= 0);
    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    REQUIRE(fixture.screen->confirm().is_open());
    CHECK(fixture.screen->confirm().title() == "Turn off the passcode?");
    CHECK(fixture.screen->confirm().detail() ==
          "Every scene folder becomes visible again.");
    CHECK(fixture.screen->confirm().focus() == 0);  // Cancel first

    fixture.screen->update(fixture.context, press(splat::Select));  // Cancel
    CHECK_FALSE(fixture.screen->confirm().is_open());
    CHECK(std::filesystem::exists(fixture.state_file()));

    fixture.screen->set_focus(row);
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.screen->update(fixture.context, press(splat::Right));   // focus Confirm
    fixture.screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::intro);
    CHECK_FALSE(std::filesystem::exists(fixture.state_file()));
    CHECK_FALSE(splat::current_visibility()->enabled);
    CHECK(splat::current_visibility()->allows_album("Family B"));
    CHECK(fixture.has_toast("Passcode off"));
    CHECK(fixture.has_log("ui: visible folders: passcode off"));
}

TEST_CASE("ui_visibility leaving the list locks it") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");
    REQUIRE(fixture.screen->unlocked());

    // Circle: the screen locks and the app pops it (handle_back returns false).
    CHECK_FALSE(fixture.screen->handle_back(fixture.context));
    CHECK_FALSE(fixture.screen->unlocked());
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    // Destroyed and opened again: the passcode is asked again.
    fixture.stack.pop();
    fixture.open();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::locked);
    CHECK_FALSE(fixture.screen->unlocked());
}

TEST_CASE("ui_visibility options from the list pops to settings and destroys it") {
    splat_test::TempDir dir{"ui-vis-app"};
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::FakeCaptureService captures{dir.path() / "captures"};
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.paths.scenes = dir.path() / "scenes";
    services.paths.inbox = dir.path() / "inbox";
    services.paths.state = dir.path() / "state";
    services.jobs = &jobs;
    services.models = &models;
    services.captures = &captures;
    services.locations = &locations;
    splat::UiApp app(services);
    splat::register_default_screens(app.registry());
    app.start();
    app.update(splat::UiInput{});
    app.update(press(splat::Menu));
    REQUIRE(app.nav().top_id() == splat::ScreenId::Settings);
    for (int i = 0; i < 11; ++i) app.update(press(splat::Down));  // Quit row is last
    // From Settings, open the Visible folders row (one Up from Quit).
    app.update(press(splat::Up));
    app.update(splat::UiInput{});
    app.update(press(splat::Select));
    REQUIRE(app.nav().top_id() == splat::ScreenId::VisibleFolders);
    CHECK(app.nav().size() == 3);
    // Options closes Settings and every screen above it (UiApp's existing rule),
    // so the Visible folders object is destroyed: no unlocked list survives the
    // route, and the next open asks for the passcode again.
    app.update(press(splat::Menu));
    CHECK(app.nav().top_id() == splat::ScreenId::Albums);
    CHECK(app.nav().size() == 1);
    app.update(press(splat::Menu));
    REQUIRE(app.nav().top_id() == splat::ScreenId::Settings);
    for (int i = 0; i < 10; ++i) app.update(press(splat::Down));  // 0.6.0: Output folder row
    app.update(press(splat::Select));
    REQUIRE(app.nav().top_id() == splat::ScreenId::VisibleFolders);
    const auto* reopened =
        static_cast<const splat::VisibleFoldersScreen*>(app.nav().top());
    CHECK(reopened->stage() == splat::VisibleFoldersScreen::Stage::intro);
}

TEST_CASE("ui_visibility a damaged state file is said, and nothing is visible") {
    Fixture fixture;
    {
        std::ofstream out(splat::visible_folders_path(fixture.services.paths.state),
                          std::ios::binary | std::ios::trunc);
        out << "{ this is not json";
    }
    const Visibility loaded =
        splat::load_visibility(fixture.services.paths.state, fixture.visibility_context()).value();
    REQUIRE(loaded.damaged);
    fixture.install(loaded);
    fixture.open();
    CHECK(fixture.screen->stage() == VisibleFoldersScreen::Stage::damaged);
    CHECK(fixture.screen->names().empty());  // no action: only a PC delete helps
    const std::vector<splat::Hint> hints = fixture.screen->hints();
    REQUIRE(hints.size() == 1);
    CHECK(hints[0].button == "Circle");
    CHECK_FALSE(splat::current_visibility()->allows_album("Family A"));
    CHECK_FALSE(fixture.screen->handle_back(fixture.context));

    // Settings says Protected, never "Off".
    splat::SettingsScreen settings;
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(settings.list().rows()[splat::SettingsScreen::VisibleFolders].right == "Protected");
}

TEST_CASE("ui_visibility no hidden name reaches a toast or the log") {
    Fixture fixture;
    Visibility state = Visibility::all_visible(fixture.visibility_context());
    REQUIRE(state.enable(fixture.services.paths.state, "2468").ok());
    state.allowed = {VisibilityKey::album("Family A")};
    REQUIRE(state.save_allowed(fixture.services.paths.state).ok());
    fixture.install(state);
    fixture.open();
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("2468");

    // Every folder row is ticked and unticked once; none of those saves may
    // name a folder or the passcode.
    for (std::size_t index = 0; index < fixture.screen->names().size(); ++index) {
        if (fixture.screen->key_at(static_cast<int>(index)) == nullptr) continue;
        fixture.screen->set_focus(static_cast<int>(index));
        fixture.screen->update(fixture.context, press(splat::Select));
        fixture.screen->set_focus(static_cast<int>(index));
        fixture.screen->update(fixture.context, press(splat::Select));
    }
    // The wrong-passcode toast is exactly the one wording.
    fixture.screen->set_focus(fixture.row_named("Change passcode"));
    fixture.screen->update(fixture.context, press(splat::Select));
    fixture.answer("0000");
    CHECK(fixture.has_toast("Wrong passcode"));
    for (const std::string& toast : fixture.toasts) {
        CHECK(toast.find("Family B") == std::string::npos);
    }
    for (const std::string& line : fixture.log) {
        CHECK(line.find("Family B") == std::string::npos);
    }
}
