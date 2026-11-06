// The photo picker while a visible-folders passcode is set: stage U1 says the
// picker is not filtered. Every source tab, console folder, location and photo
// is listed with nothing ticked, the basket keeps its photos, and only the
// scene shelf (the album list) hides anything.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/gallery.h"
#include "gallery/visibility.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/photos.h"
#include "ui/screen_stack.h"

namespace {

using splat::Path;
using splat::Visibility;
using splat::VisibilityContext;
using splat::VisibilityKey;

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

void touch(const Path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "x";
}

struct Fixture {
    splat_test::TempDir dir{"ui-photos-vis"};
    Path base = dir.path();
    Path data = base / "data";
    Path app = data / "ps5-splat-vr";
    Path inbox = app / "inbox";
    Path scenes = app / "scenes";
    Path mnt = base / "mnt";
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::FakeCaptureService captures{base / "captures"};
    splat::FakeLocationService locations;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::PhotosScreen screen;
    std::vector<std::string> toasts;
    std::unique_ptr<splat::VisibilityScope> scope;

    Fixture() {
        services.paths.inbox = inbox;
        services.paths.scenes = scenes;
        services.paths.state = app / "state";
        services.paths.mnt = mnt;
        services.paths.data = data;
        services.jobs = &jobs;
        services.models = &models;
        services.captures = &captures;
        services.locations = &locations;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        stack.set_context(&context);
        std::error_code error;
        std::filesystem::create_directories(inbox, error);
        std::filesystem::create_directories(scenes / "Family A", error);
        std::filesystem::create_directories(scenes / "Family B", error);
        std::filesystem::create_directories(data / "Photos" / "fam", error);
        std::filesystem::create_directories(data / "Hidden", error);
        touch(data / "root.jpg");
        touch(data / "Photos" / "a.jpg");
        touch(data / "Photos" / "fam" / "deep.jpg");
        touch(data / "Hidden" / "b.jpg");
        touch(mnt / "usb0" / "stick.jpg");
        install(Visibility::all_visible(visibility_context()));
    }

    VisibilityContext visibility_context() const {
        VisibilityContext out;
        out.scenes_root = scenes;
        out.inbox = inbox;
        out.mnt = mnt;
        out.data = data;
        out.app_data = app;
        return out;
    }

    void install(Visibility state) {
        scope.reset();
        scope = std::make_unique<splat::VisibilityScope>(std::move(state));
    }

    // A policy that ticks exactly `keys`.
    void restrict(std::initializer_list<VisibilityKey> keys) {
        Visibility state = Visibility::all_visible(visibility_context());
        state.enabled = true;
        for (const VisibilityKey& key : keys) state.allowed.push_back(key);
        std::sort(state.allowed.begin(), state.allowed.end());
        install(std::move(state));
    }

    void enter() {
        splat::ScreenArgs args;
        args.a = "trip";
        screen.on_enter(context, args);
        screen.update(context, splat::UiInput{});
    }

    void idle() { screen.update(context, splat::UiInput{}); }

    std::vector<std::string> tab_titles() const {
        std::vector<std::string> out;
        for (const splat::PhotoTab& tab : screen.tabs()) out.push_back(tab.title);
        return out;
    }

    int tab_index(const std::string& id) const {
        for (std::size_t index = 0; index < screen.tabs().size(); ++index) {
            if (screen.tabs()[index].id == id) return static_cast<int>(index);
        }
        return -1;
    }

    std::vector<std::string> cell_captions() const {
        std::vector<std::string> out;
        for (const splat::GridCell& cell : screen.grid().cells()) out.push_back(cell.caption);
        return out;
    }
};

}  // namespace

TEST_CASE("ui_photos_visibility nothing ticked still lists every source and photo") {
    Fixture fixture;
    // A passcode is set and the owner has ticked nothing. The sheet is closed;
    // the picker is not.
    fixture.restrict({});
    fixture.enter();

    CHECK(fixture.tab_titles() ==
          (std::vector<std::string>{"Inbox", "Console", "USB", "Captures", "nas.local/Photos",
                                    "+ Add network location"}));
    CHECK(fixture.tab_index("smb0") >= 0);   // the photo location is a source
    CHECK(fixture.tab_index("add_location") >= 0);
    // The inbox tab lists its file, whatever the allow-list says.
    touch(fixture.inbox / "upload.jpg");
    fixture.screen.refresh(fixture.context);
    fixture.screen.set_tab(fixture.context, fixture.tab_index("inbox"));
    fixture.idle();
    CHECK(fixture.cell_captions() == std::vector<std::string>{"upload.jpg"});
}

TEST_CASE("ui_photos_visibility nothing ticked still lists every console folder and photo") {
    Fixture fixture;
    fixture.restrict({});
    fixture.enter();
    const int console = fixture.tab_index("console");
    REQUIRE(console >= 0);
    fixture.screen.set_tab(fixture.context, console);
    fixture.idle();
    // One storage: the browser starts inside it and shows every folder and the
    // file directly in the root (nothing is hidden by the passcode).
    CHECK(fixture.screen.console_dir() == fixture.data);
    std::vector<std::string> captions = fixture.cell_captions();
    std::sort(captions.begin(), captions.end());
    CHECK(captions == (std::vector<std::string>{"Hidden", "Photos", "root.jpg"}));

    // The unticked folder opens and lists its photos.
    const int photos = [&] {
        for (std::size_t index = 0; index < fixture.cell_captions().size(); ++index) {
            if (fixture.cell_captions()[index] == "Photos") return static_cast<int>(index);
        }
        return -1;
    }();
    REQUIRE(photos >= 0);
    for (int step = 0; step < photos; ++step) fixture.screen.update(fixture.context, press(splat::Right));
    fixture.screen.update(fixture.context, press(splat::Select));
    fixture.idle();
    CHECK(fixture.screen.console_dir() == fixture.data / "Photos");
    std::vector<std::string> inside = fixture.cell_captions();
    std::sort(inside.begin(), inside.end());
    CHECK(inside == (std::vector<std::string>{"a.jpg", "fam"}));
}

TEST_CASE("ui_photos_visibility the basket keeps a photo from an unticked source") {
    Fixture fixture;
    touch(fixture.inbox / "upload.jpg");
    fixture.restrict({});
    fixture.enter();
    fixture.idle();
    REQUIRE(fixture.cell_captions() == std::vector<std::string>{"upload.jpg"});
    fixture.screen.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    // The provenance key still travels into the job (information for meta.json).
    CHECK(fixture.screen.selection()[0].source_folder == "source:inbox");
    // A refresh no longer drops it: no source is hidden.
    fixture.screen.refresh(fixture.context);
    CHECK(fixture.screen.selection().size() == 1);
}

TEST_CASE("ui_photos_visibility the scene shelf still hides an unticked album") {
    Fixture fixture;
    const std::vector<std::string> with_none = [&] {
        fixture.restrict({});
        std::vector<std::string> names;
        for (const splat::AlbumEntry& album : splat::list_albums(fixture.scenes)) {
            names.push_back(album.name);
        }
        return names;
    }();
    CHECK(with_none.empty());

    fixture.restrict({VisibilityKey::album("Family A")});
    std::vector<std::string> names;
    for (const splat::AlbumEntry& album : splat::list_albums(fixture.scenes)) {
        names.push_back(album.name);
    }
    CHECK(names == std::vector<std::string>{"Family A"});

    // Without a passcode every folder is there, as before.
    fixture.install(Visibility::all_visible(fixture.visibility_context()));
    names.clear();
    for (const splat::AlbumEntry& album : splat::list_albums(fixture.scenes)) {
        names.push_back(album.name);
    }
    CHECK(names == (std::vector<std::string>{splat::kAllScenesAlbumName, "Family A", "Family B"}));
}
