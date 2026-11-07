// The photo picker browses a network location exactly like a local folder
// (0.6.0): the top of the share shows its subfolders as folder tiles first, then
// its photos; Cross opens a folder, Circle goes back up, the place is shown under
// the tab, and each visit is one directory listing over the VFS - the share's
// tree is never walked. The share here is an in-memory tree that records every
// list() call it receives.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/path.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/location_service.h"
#include "ui/screen_stack.h"
#include "ui/screens/photos.h"
#include "vfs/io.h"

namespace {

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

bool has_hint(const std::vector<splat::Hint>& hints, const std::string& button,
              const std::string& label) {
    for (const splat::Hint& hint : hints) {
        if (hint.button == button && hint.label == label) return true;
    }
    return false;
}

// The picker with one network location mounted from `share`. smb0 is the fake
// service's photo source at "scenes"; `root_share` turns smb1 into a photo
// source at the share's root (an empty path).
struct NetworkPicker {
    splat_test::TempDir dir{"ui-photos-network"};
    splat::FakeLocationService locations;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::PhotosScreen* photos = nullptr;
    std::shared_ptr<splat_test::TreeFileSystem> share;
    std::string id;

    explicit NetworkPicker(const std::string& location_id) : id(location_id) {
        share = std::make_shared<splat_test::TreeFileSystem>(id);
        splat::vfs::file_systems().add(share);

        std::error_code error;
        std::filesystem::create_directories(dir.path() / "inbox", error);
        std::filesystem::create_directories(dir.path() / "scenes", error);
        std::filesystem::create_directories(dir.path() / "mnt", error);
        if (id == "smb1") {
            splat::LocationView view;
            for (const splat::LocationView& row : locations.list()) {
                if (row.id == "smb1") view = row;
            }
            view.photo_source = true;
            REQUIRE(locations.update(view).ok());
        }
        services.locations = &locations;
        services.paths.inbox = dir.path() / "inbox";
        services.paths.scenes = dir.path() / "scenes";
        services.paths.mnt = dir.path() / "mnt";
        services.paths.data = dir.path() / "data";
        services.paths.state = dir.path() / "state";
        context.content = splat::Rect{16, 78, 1248, 586};
        stack.set_context(&context);
        registry.add(splat::ScreenId::Photos, [] {
            return std::unique_ptr<splat::Screen>(new splat::PhotosScreen());
        });
        stack.push(splat::ScreenId::Photos);
        photos = dynamic_cast<splat::PhotosScreen*>(stack.top());
        REQUIRE(photos != nullptr);
    }

    ~NetworkPicker() { splat::vfs::file_systems().remove(id); }

    // Opens the location's tab (its first listing is requested, not finished).
    void open_tab() {
        int tab = -1;
        for (std::size_t index = 0; index < photos->tabs().size(); ++index) {
            if (photos->tabs()[index].id == id) tab = static_cast<int>(index);
        }
        REQUIRE_MESSAGE(tab >= 0, "no photo tab for the location");
        photos->set_tab(context, tab);
    }

    // Pumps frames until the grid has `count` tiles (the worker publishes in
    // tick(), which never blocks).
    void wait_cells(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (photos->grid().cells().size() != count &&
               std::chrono::steady_clock::now() < deadline) {
            photos->update(context, splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(photos->grid().cells().size() == count);
    }

    void wait_message() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (photos->message().empty() && std::chrono::steady_clock::now() < deadline) {
            photos->update(context, splat::UiInput{});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE_FALSE(photos->message().empty());
    }

    std::string caption(std::size_t index) const {
        return photos->grid().cells()[index].caption;
    }
    bool folder(std::size_t index) const {
        return photos->grid().cells()[index].icon == splat::GridCell::Icon::Folder;
    }
};

}  // namespace

TEST_CASE("a network photo tab shows only the top level and browses folder by folder") {
    NetworkPicker picker("smb0");
    splat_test::TreeFileSystem& share = *picker.share;
    // The share's "scenes" folder is the location's path: a mix of photos, a
    // splat, macOS metadata and a nested tree three levels deep.
    share.files["scenes/a.jpg"] = {1};
    share.files["scenes/b.png"] = {2};
    share.files["scenes/.DS_Store"] = {3};
    share.files["scenes/._a.jpg"] = {4};
    share.files["scenes/notes.txt"] = {5};
    share.files["scenes/2025/e.webp"] = {6};
    share.files["scenes/2026/d.jpg"] = {7};
    share.files["scenes/2026/july/c.jpg"] = {8};
    share.files["scenes/2026/july/day1/deep.jpg"] = {9};

    picker.open_tab();
    // Folders first, then the photos of this one folder; the macOS files and the
    // non-photo are not tiles. Nothing below the first level is listed.
    picker.wait_cells(4);
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "2025");
    CHECK(picker.folder(1));
    CHECK(picker.caption(1) == "2026");
    CHECK_FALSE(picker.folder(2));
    CHECK(picker.caption(2) == "a.jpg");
    CHECK(picker.caption(3) == "b.png");
    CHECK(share.list_calls() == std::vector<std::string>{"scenes"});
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes"));
    CHECK(picker.photos->breadcrumb().empty());
    CHECK(picker.photos->location() == "Photos on nas.local/Photos");
    CHECK(has_hint(picker.photos->hints(), "Cross", "Open"));
    CHECK(has_hint(picker.photos->hints(), "Circle", "Back"));  // the top of the share

    // Cross on the first folder opens it: one more listing, its one photo.
    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_cells(1);
    CHECK(picker.caption(0) == "e.webp");
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes/2025"));
    CHECK(picker.photos->breadcrumb() == std::vector<std::string>{"2025"});
    CHECK(picker.photos->location() == "2025");
    CHECK(has_hint(picker.photos->hints(), "Circle", "Up"));
    CHECK(share.list_calls() == std::vector<std::string>{"scenes", "scenes/2025"});

    // Back goes up one level and lists the top again, with the focus on the
    // folder that was left.
    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(4);
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes"));
    CHECK(picker.photos->grid().focus() == 0);
    CHECK(picker.photos->breadcrumb().empty());

    // Into 2026 (Right moves the focus), then into its subfolder: the crumb grows
    // by one folder per level.
    picker.photos->update(picker.context, press(splat::Right));
    CHECK(picker.photos->grid().focus() == 1);
    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_cells(2);
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "july");
    CHECK(picker.caption(1) == "d.jpg");
    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "day1");
    CHECK(picker.caption(1) == "c.jpg");
    CHECK(picker.photos->breadcrumb() == std::vector<std::string>{"2026", "july"});

    // Circle walks back up through each level and, at the share's top, leaves
    // the picker (handle_back answers false) instead of going above its path.
    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes/2026"));
    CHECK(picker.photos->grid().focus() == 0);  // back on "july"
    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(4);
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes"));
    CHECK(picker.photos->grid().focus() == 1);  // back on "2026"
    CHECK_FALSE(picker.photos->handle_back(picker.context));

    // One listing per visit, always exactly the folder that was entered: the
    // "day1" folder below "july" was never opened, so it was never listed.
    CHECK(share.list_calls() ==
          std::vector<std::string>{"scenes", "scenes/2025", "scenes", "scenes/2026",
                                   "scenes/2026/july", "scenes/2026", "scenes"});
}

TEST_CASE("a network photo tab at the share's root goes back to the root, not above it") {
    // smb1 has an empty path: the tab's top is "smb1:/", the case where a plain
    // parent_path() would turn "smb1:/x" into "smb1:".
    NetworkPicker picker("smb1");
    splat_test::TreeFileSystem& share = *picker.share;
    share.files["top.png"] = {1};
    share.files["x/y/z.jpg"] = {2};
    share.files["x/w.jpg"] = {3};

    picker.open_tab();
    picker.wait_cells(2);
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "x");
    CHECK(picker.caption(1) == "top.png");
    CHECK(picker.photos->browse_dir() == splat::Path("smb1:/"));

    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "y");
    CHECK(picker.caption(1) == "w.jpg");
    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_cells(1);
    CHECK(picker.caption(0) == "z.jpg");
    CHECK(picker.photos->breadcrumb() == std::vector<std::string>{"x", "y"});

    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.photos->browse_dir() == splat::Path("smb1:/x"));
    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.photos->browse_dir() == splat::Path("smb1:/"));
    CHECK_FALSE(picker.photos->handle_back(picker.context));
    CHECK(share.list_calls() ==
          std::vector<std::string>{"", "x", "x/y", "x", ""});
}

TEST_CASE("a folder of a network photo tab that cannot be read shows the share's error") {
    NetworkPicker picker("smb0");
    splat_test::TreeFileSystem& share = *picker.share;
    share.files["scenes/ok.jpg"] = {1};
    share.files["scenes/locked/p.jpg"] = {2};
    share.unreadable.insert("scenes/locked");

    picker.open_tab();
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "locked");

    // Entering it reports the share's own message, never an empty folder.
    picker.photos->update(picker.context, press(splat::Select));
    picker.wait_message();
    CHECK(picker.photos->message().find("access denied") != std::string::npos);
    CHECK(picker.photos->message().find("No folders") == std::string::npos);
    CHECK(picker.photos->grid().cells().empty());
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes/locked"));

    // Circle still goes back up from there.
    CHECK(picker.photos->handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.photos->browse_dir() == splat::Path("smb0:/scenes"));
    CHECK(picker.photos->message().empty());
}
