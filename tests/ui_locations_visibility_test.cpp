// The Locations screen and the location editor while a passcode is set: every
// location is listed and manageable (stage U1: management lives in Settings),
// the Settings count includes every location, and a hidden library opened by a
// stale path is still blocked.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/visibility.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/location_edit.h"
#include "ui/screens/locations.h"
#include "ui/screens/scenes.h"
#include "ui/screens/settings.h"
#include "ui/screen_stack.h"

namespace {

using splat::Path;
using splat::Visibility;
using splat::VisibilityContext;
using splat::VisibilityKey;

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

struct Fixture {
    splat_test::TempDir dir{"ui-loc-vis"};
    Path base = dir.path();
    splat::FakeLocationService locations;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    std::unique_ptr<splat::VisibilityScope> scope;

    Fixture() {
        services.paths.scenes = base / "scenes";
        services.paths.inbox = base / "data" / "ps5-splat-vr" / "inbox";
        services.paths.state = base / "data" / "ps5-splat-vr" / "state";
        services.paths.mnt = base / "mnt";
        services.paths.data = base / "data";
        services.locations = &locations;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        stack.set_context(&context);
        registry.add(splat::ScreenId::Locations,
                     [] { return std::make_unique<splat::LocationsScreen>(); });
        registry.add(splat::ScreenId::LocationEdit,
                     [] { return std::make_unique<splat::LocationEditScreen>(); });
        install(Visibility::all_visible(splat::VisibilityContext{}));
    }

    void install(Visibility state) {
        scope.reset();
        scope = std::make_unique<splat::VisibilityScope>(std::move(state));
    }

    void enable(std::initializer_list<VisibilityKey> keys) {
        Visibility state = Visibility::all_visible(splat::VisibilityContext{});
        state.enabled = true;
        for (const VisibilityKey& key : keys) state.allowed.push_back(key);
        std::sort(state.allowed.begin(), state.allowed.end());
        install(std::move(state));
    }

    splat::LocationsScreen* open_locations() {
        stack.push(splat::ScreenId::Locations);
        return static_cast<splat::LocationsScreen*>(stack.top());
    }

    bool has_toast(const std::string& text) const {
        for (const std::string& entry : toasts) {
            if (entry == text) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("ui_locations_visibility settings locations is editable with a passcode set") {
    Fixture fixture;
    fixture.enable({VisibilityKey::location("smb0")});
    splat::LocationsScreen* screen = fixture.open_locations();
    REQUIRE(screen != nullptr);
    // Every configured location is listed, whatever is ticked: the passcode
    // hides a location's scenes, never its configuration (stage U1).
    const std::vector<std::string> names = screen->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "nas.local/Photos");
    CHECK(names[1] == "192.168.1.20/Scenes");
    CHECK(names.back() == splat::LocationsScreen::kAddLabel);

    // Triangle edits the unticked row: the editor opens with its fields, not a
    // "enter the passcode" message.
    screen->set_focus(1);
    screen->update(fixture.context, press(splat::Context));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::LocationEdit);
    auto* edit = static_cast<splat::LocationEditScreen*>(fixture.stack.top());
    CHECK(edit->title() == "Edit location");
    CHECK(edit->list().rows().size() > 3);
    fixture.stack.pop();

    // Square asks to delete, and the card's confirm really removes it.
    screen = static_cast<splat::LocationsScreen*>(fixture.stack.top());
    screen->set_focus(1);
    screen->update(fixture.context, press(splat::Secondary));
    REQUIRE(screen->confirm().is_open());
    screen->update(fixture.context, press(splat::Right));   // focus Confirm
    screen->update(fixture.context, press(splat::Select));
    CHECK(fixture.locations.list().size() == 1);
    CHECK(screen->names().size() == 2);  // one location + Add

    // Cross on the Add row opens a blank editor.
    screen->set_focus(static_cast<int>(screen->names().size()) - 1);
    screen->update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::LocationEdit);
    auto* add = static_cast<splat::LocationEditScreen*>(fixture.stack.top());
    CHECK(add->title() == "Add location");
    CHECK(add->list().rows().size() > 3);
}

TEST_CASE("ui_locations_visibility settings counts every location") {
    Fixture fixture;
    splat::SettingsScreen settings;
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(settings.list().rows()[splat::SettingsScreen::Locations].right == "2 locations");

    // A passcode and one ticked location do not shrink the count: the row is
    // about the configured shares, not about what a visitor may see.
    fixture.enable({VisibilityKey::location("smb0")});
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(settings.list().rows()[splat::SettingsScreen::Locations].right == "2 locations");

    fixture.enable({});
    settings.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(settings.list().rows()[splat::SettingsScreen::Locations].right == "2 locations");
}

TEST_CASE("ui_locations_visibility the editor opens with a passcode set") {
    Fixture fixture;
    fixture.enable({VisibilityKey::location("smb0")});
    splat::LocationEditScreen edit;
    splat::ScreenArgs args;
    args.a = "smb0";
    edit.on_enter(fixture.context, args);
    CHECK(edit.title() == "Edit location");
    CHECK_FALSE(edit.list().rows().empty());
    CHECK(edit.draft().host == "nas.local");
    // The hints offer the same actions as without a passcode.
    const std::vector<splat::Hint> hints = edit.hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Cross");
}

TEST_CASE("ui_locations_visibility the editor saves with a passcode set") {
    Fixture fixture;
    fixture.enable({VisibilityKey::location("smb0")});
    splat::ScreenArgs args;
    args.a = "smb0";  // the ticked row, edited in place
    REQUIRE(fixture.stack.push(splat::ScreenId::LocationEdit, args));
    auto* edit = static_cast<splat::LocationEditScreen*>(fixture.stack.top());
    const auto row_of = [edit](splat::LocationEditScreen::Field field) {
        for (int row = 0; row < 32; ++row) {
            const splat::LocationEditScreen::Field at = edit->field_at(row);
            if (at == field) return row;
            if (at == splat::LocationEditScreen::Field::Save) break;
        }
        return -1;
    };

    // A real toggle on the screen, then Save: the editor writes the store. The
    // old read-only mode (stage H) refused this with kLockedMessage.
    REQUIRE(edit->draft().photo_source);
    edit->activate(fixture.context, row_of(splat::LocationEditScreen::Field::PhotoSource));
    CHECK_FALSE(edit->draft().photo_source);
    edit->activate(fixture.context, row_of(splat::LocationEditScreen::Field::Save));
    CHECK(fixture.has_toast("Saved smb0"));
    const std::vector<splat::LocationView> listed = fixture.locations.list();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == "smb0");
    CHECK_FALSE(listed[0].photo_source);
}

TEST_CASE("ui_locations_visibility a hidden library opened by a stale path is blocked") {
    Fixture fixture;
    fixture.enable({VisibilityKey::location("smb0")});
    splat::ScenesScreen scenes;
    splat::ScreenArgs args;
    args.a.clear();
    args.b = "smb1:/scenes";  // the hidden location
    scenes.on_enter(fixture.context, args);
    CHECK(scenes.blocked());
    CHECK(scenes.title() == "Folder");  // the location's name never appears
    CHECK(scenes.names().empty());
    CHECK(scenes.hints().size() == 1);

    // The ticked one opens normally (the fake VFS has no adapter here, so the
    // listing fails, but the screen is not blocked and keeps its real title).
    splat::ScenesScreen visible;
    args.b = "smb0:/scenes";
    visible.on_enter(fixture.context, args);
    CHECK_FALSE(visible.blocked());
    CHECK(visible.title() == "Unsorted");
}
