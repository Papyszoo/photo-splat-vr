// The photo picker's Inbox and USB tabs are folder browsers, like Console and a
// network location (0.6.0, user decision 2026-11-07: "folder structure just like
// finder or windows explorer"): the top of the source shows its subfolders as
// folder tiles first, then that one folder's photos and splat files; Cross opens
// a folder, Circle goes back up (and leaves the picker at the top), the place is
// shown under the tab, and each visit is one directory listing - the tree below
// is never walked and Square on a folder tile does nothing.
//
// A local folder cannot report which listings were made of it, so the exact
// call sequences are checked on an in-memory tree (tests/tree_filesystem.h)
// standing in for the stick or the Inbox; the same screen code runs on both. The
// cases on real folders check what only the real disk can: the folders on it,
// the stick tabs, the basket and the empty states.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/path.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/screen_stack.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screens/photos.h"
#include "vfs/io.h"

using splat::Path;

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

// A file the picker only lists (nothing here opens it).
void write_file(const Path& path, const std::string& content = "x") {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    REQUIRE(static_cast<bool>(out));
}

struct Picker {
    splat_test::TempDir dir{"ui-photos-browse"};
    splat::FakeJobService jobs;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::PhotosScreen screen;
    std::vector<std::string> toasts;
    std::shared_ptr<splat_test::TreeFileSystem> tree;

    Picker() {
        services.paths.inbox = dir.path() / "inbox";
        services.paths.mnt = dir.path() / "mnt";
        services.paths.scenes = dir.path() / "scenes";
        services.paths.state = dir.path() / "state";
        services.jobs = &jobs;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        registry.add(splat::ScreenId::ConvertDialog,
                     [] { return std::make_unique<splat::ConvertDialog>(); });
        stack.set_context(&context);
    }

    ~Picker() {
        if (tree) splat::vfs::file_systems().remove(tree->id());
    }

    // The in-memory tree, mounted as "fake0": the Inbox or the mount table can
    // point into it ("fake0:/inbox", "fake0:/mnt").
    splat_test::TreeFileSystem& mount_tree() {
        tree = std::make_shared<splat_test::TreeFileSystem>("fake0");
        splat::vfs::file_systems().add(tree);
        return *tree;
    }

    void enter() {
        splat::ScreenArgs args;
        args.a = "trip";
        screen.on_enter(context, args);
    }
    void update(const splat::UiInput& input) { screen.update(context, input); }
    void idle() { screen.update(context, splat::UiInput{}); }

    int tab_of(const std::string& id) const {
        for (std::size_t index = 0; index < screen.tabs().size(); ++index) {
            if (screen.tabs()[index].id == id) return static_cast<int>(index);
        }
        return -1;
    }

    // Pumps frames until the grid has `count` tiles (a listing on the worker
    // publishes in tick(), which never blocks).
    void wait_cells(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (screen.grid().cells().size() != count &&
               std::chrono::steady_clock::now() < deadline) {
            idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(screen.grid().cells().size() == count);
    }

    void wait_message() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (screen.message().empty() && std::chrono::steady_clock::now() < deadline) {
            idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE_FALSE(screen.message().empty());
    }

    std::string caption(std::size_t index) const { return screen.grid().cells()[index].caption; }
    bool folder(std::size_t index) const {
        return screen.grid().cells()[index].icon == splat::GridCell::Icon::Folder;
    }
};

}  // namespace

TEST_CASE("the Inbox shows only its top level and browses folder by folder") {
    Picker picker;
    splat_test::TreeFileSystem& inbox = picker.mount_tree();
    picker.services.paths.inbox = splat::vfs::join("fake0", "inbox");
    // A mix of photos, a splat, macOS metadata, a non-photo and a nested tree
    // three levels deep.
    inbox.files["inbox/a.jpg"] = {1};
    inbox.files["inbox/b.png"] = {2};
    inbox.files["inbox/robot.spz"] = {3};
    inbox.files["inbox/.DS_Store"] = {4};
    inbox.files["inbox/._a.jpg"] = {5};
    inbox.files["inbox/notes.txt"] = {6};
    inbox.files["inbox/2025/e.webp"] = {7};
    inbox.files["inbox/2026/d.jpg"] = {8};
    inbox.files["inbox/2026/july/c.jpg"] = {9};
    inbox.files["inbox/2026/july/day1/deep.jpg"] = {10};

    picker.enter();
    REQUIRE(picker.tab_of("inbox") == 0);
    // The tab's number counts the files at its top, never below it.
    CHECK(picker.screen.tabs()[0].count == 3);
    // Folders first, then the photos of this one folder by name, then its splat
    // files; the macOS files and the non-photo are not tiles.
    picker.wait_cells(5);
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "2025");
    CHECK(picker.folder(1));
    CHECK(picker.caption(1) == "2026");
    CHECK(picker.caption(2) == "a.jpg");
    CHECK(picker.caption(3) == "b.png");
    CHECK(picker.caption(4) == "robot.spz");
    CHECK(picker.screen.grid().cells()[4].badge == "SPLAT");
    // The tab count, then the one listing of the top. Nothing below it.
    CHECK(inbox.list_calls() == std::vector<std::string>{"inbox", "inbox"});
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox"));
    CHECK(picker.screen.breadcrumb().empty());
    // At the top the line says where to upload, as before.
    CHECK(picker.screen.location() == "Upload photos by FTP to fake0:/inbox");
    CHECK(has_hint(picker.screen.hints(), "Cross", "Open"));
    CHECK(has_hint(picker.screen.hints(), "Circle", "Back"));
    CHECK_FALSE(has_hint(picker.screen.hints(), "Square", "Import splats"));
    CHECK_FALSE(picker.screen.handle_back(picker.context));  // the top leaves the picker

    // Cross on the first folder opens it: one more listing, its one photo.
    inbox.forget_list_calls();
    picker.update(press(splat::Select));
    picker.wait_cells(1);
    CHECK(picker.caption(0) == "e.webp");
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox/2025"));
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"2025"});
    CHECK(picker.screen.location() == "2025");
    CHECK(has_hint(picker.screen.hints(), "Circle", "Up"));
    CHECK(inbox.list_calls() == std::vector<std::string>{"inbox/2025"});

    // Circle goes up one level, the focus on the folder that was left.
    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(5);
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox"));
    CHECK(picker.screen.grid().focus() == 0);
    CHECK(picker.screen.location() == "Upload photos by FTP to fake0:/inbox");

    // Into 2026, then its subfolder: the crumb grows by one folder per level.
    picker.update(press(splat::Right));
    CHECK(picker.screen.grid().focus() == 1);
    picker.update(press(splat::Select));
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "july");
    CHECK(picker.caption(1) == "d.jpg");
    picker.update(press(splat::Select));
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "day1");
    CHECK(picker.caption(1) == "c.jpg");
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"2026", "july"});

    // Circle walks back up through each level with the focus restored, and at the
    // top leaves the picker instead of going above the Inbox.
    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox/2026"));
    CHECK(picker.screen.grid().focus() == 0);  // back on "july"
    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(5);
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox"));
    CHECK(picker.screen.grid().focus() == 1);  // back on "2026"
    CHECK_FALSE(picker.screen.handle_back(picker.context));

    // One listing per visit, always exactly the folder that was entered: the
    // "day1" folder below "july" was never opened, so it was never listed.
    CHECK(inbox.list_calls() ==
          std::vector<std::string>{"inbox/2025", "inbox", "inbox/2026", "inbox/2026/july",
                                   "inbox/2026", "inbox"});
}

TEST_CASE("a USB stick browses folder by folder and never lists below the folder entered") {
    Picker picker;
    splat_test::TreeFileSystem& tree = picker.mount_tree();
    picker.services.paths.mnt = splat::vfs::join("fake0", "mnt");
    tree.files["mnt/usb0/x.jpg"] = {1};
    tree.files["mnt/usb0/trip/y.png"] = {2};
    tree.files["mnt/usb0/trip/deep/z.jpg"] = {3};
    tree.files["mnt/usb0/trip/deep/deeper/w.jpg"] = {4};
    tree.files["mnt/usb0/export/robot.splat"] = {5};

    picker.enter();
    const int usb = picker.tab_of("usb0");
    REQUIRE(usb >= 0);
    CHECK(picker.screen.tabs()[static_cast<std::size_t>(usb)].title == "USB");

    tree.forget_list_calls();
    picker.screen.set_tab(picker.context, usb);
    picker.wait_cells(3);  // export/, trip/, x.jpg
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "export");
    CHECK(picker.caption(1) == "trip");
    CHECK(picker.caption(2) == "x.jpg");
    CHECK(tree.list_calls() == std::vector<std::string>{"mnt/usb0"});
    CHECK(picker.screen.browse_dir() == Path("fake0:/mnt/usb0"));
    CHECK(picker.screen.location() == "Photos on the USB drive");
    CHECK(has_hint(picker.screen.hints(), "Circle", "Back"));
    CHECK_FALSE(picker.screen.handle_back(picker.context));

    picker.update(press(splat::Right));  // trip/
    picker.update(press(splat::Select));
    picker.wait_cells(2);  // deep/, y.png
    CHECK(picker.caption(0) == "deep");
    CHECK(picker.caption(1) == "y.png");
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"trip"});
    CHECK(picker.screen.location() == "trip");
    CHECK(has_hint(picker.screen.hints(), "Circle", "Up"));
    picker.update(press(splat::Select));  // deep/
    picker.wait_cells(2);                 // deeper/, z.jpg
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"trip", "deep"});

    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.screen.browse_dir() == Path("fake0:/mnt/usb0/trip"));
    CHECK(picker.screen.grid().focus() == 0);  // back on "deep"
    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(3);
    CHECK(picker.screen.browse_dir() == Path("fake0:/mnt/usb0"));
    CHECK(picker.screen.grid().focus() == 1);  // back on "trip"
    CHECK_FALSE(picker.screen.handle_back(picker.context));
    CHECK(picker.screen.location() == "Photos on the USB drive");

    // Exactly the folders that were visited; "deeper" and "export" were never
    // opened, so never listed.
    CHECK(tree.list_calls() ==
          std::vector<std::string>{"mnt/usb0", "mnt/usb0/trip", "mnt/usb0/trip/deep",
                                   "mnt/usb0/trip", "mnt/usb0"});
}

TEST_CASE("a folder of the Inbox that cannot be read shows the file system's error") {
    Picker picker;
    splat_test::TreeFileSystem& inbox = picker.mount_tree();
    picker.services.paths.inbox = splat::vfs::join("fake0", "inbox");
    inbox.files["inbox/ok.jpg"] = {1};
    inbox.files["inbox/locked/p.jpg"] = {2};
    inbox.unreadable.insert("inbox/locked");

    picker.enter();
    picker.wait_cells(2);
    CHECK(picker.caption(0) == "locked");
    picker.update(press(splat::Select));
    picker.wait_message();
    CHECK(picker.screen.message().find("access denied") != std::string::npos);
    CHECK(picker.screen.grid().cells().empty());
    // Circle still goes back up from there.
    CHECK(picker.screen.handle_back(picker.context));
    picker.wait_cells(2);
    CHECK(picker.screen.browse_dir() == Path("fake0:/inbox"));
    CHECK(picker.screen.message().empty());
}

TEST_CASE("the Inbox on a real folder: top level, enter, up, and the basket across folders") {
    Picker picker;
    const Path inbox = picker.services.paths.inbox;
    write_file(inbox / "b.jpg");
    write_file(inbox / "a.jpg");
    write_file(inbox / "2026" / "july" / "c.jpg");
    write_file(inbox / "2026" / "july" / "day1" / "deep.jpg");
    write_file(inbox / "2026" / "d.png");

    picker.enter();
    picker.idle();
    // Name order, folders first; nothing from 2026/ appears at the top.
    REQUIRE(picker.screen.grid().cells().size() == 3);
    CHECK(picker.folder(0));
    CHECK(picker.caption(0) == "2026");
    CHECK(picker.caption(1) == "a.jpg");
    CHECK(picker.caption(2) == "b.jpg");
    CHECK(picker.screen.browse_dir() == inbox);

    picker.update(press(splat::Select));  // into 2026/
    picker.idle();
    CHECK(picker.screen.browse_dir() == inbox / "2026");
    REQUIRE(picker.screen.grid().cells().size() == 2);  // july/, d.png
    CHECK(picker.caption(0) == "july");
    CHECK(picker.caption(1) == "d.png");
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"2026"});

    // Pick a photo here; the basket survives going up, and Square converts it.
    picker.update(press(splat::Right));
    picker.update(press(splat::Select));
    REQUIRE(picker.screen.selection().size() == 1);
    CHECK(picker.screen.selection()[0].path == inbox / "2026" / "d.png");
    CHECK(picker.screen.handle_back(picker.context));
    CHECK(picker.screen.browse_dir() == inbox);
    CHECK(picker.screen.grid().focus() == 0);  // on 2026/, the folder we left
    CHECK(picker.screen.selection().size() == 1);
    CHECK(has_hint(picker.screen.hints(), "Square", "Convert (1)"));
    CHECK_FALSE(picker.screen.handle_back(picker.context));

    // Another tab starts its own basket.
    picker.update(press(splat::PageNext));
    CHECK(picker.screen.selection().empty());
}

TEST_CASE("Square on a folder tile does nothing on the Inbox") {
    Picker picker;
    const Path inbox = picker.services.paths.inbox;
    write_file(inbox / "set" / "robot.spz");
    write_file(inbox / "set" / "more" / "other.spz");
    write_file(inbox / "photo.jpg");

    picker.enter();
    picker.idle();
    REQUIRE(picker.screen.grid().cells().size() == 2);
    REQUIRE(picker.folder(0));
    // The hint bar does not offer a folder action on a folder tile.
    for (const splat::Hint& hint : picker.screen.hints()) {
        CHECK(hint.button != "Square");
        CHECK(hint.label.find("Import") == std::string::npos);
    }

    picker.update(press(splat::Secondary));
    CHECK(picker.jobs.last_import().empty());
    CHECK(picker.jobs.snapshot().empty());
    CHECK(picker.stack.top_id() != splat::ScreenId::ConvertDialog);
    CHECK(picker.toasts.empty());
    CHECK(picker.screen.selection().empty());
    CHECK(picker.screen.browse_dir() == inbox);  // and it did not enter the folder
}

TEST_CASE("an empty folder inside the Inbox is just an empty folder, and Circle goes up") {
    Picker picker;
    const Path inbox = picker.services.paths.inbox;
    std::filesystem::create_directories(inbox / "empty");
    write_file(inbox / "a.jpg");

    picker.enter();
    picker.idle();
    picker.update(press(splat::Select));  // empty/
    picker.idle();
    CHECK(picker.screen.browse_dir() == inbox / "empty");
    CHECK(picker.screen.grid().cells().empty());
    CHECK(picker.screen.message() == "No folders or photos here");
    CHECK(picker.screen.handle_back(picker.context));
    picker.idle();
    CHECK(picker.screen.browse_dir() == inbox);
    CHECK(picker.screen.message().empty());
    CHECK(picker.screen.grid().cells().size() == 2);
}

TEST_CASE("each USB stick is its own tab with its own top") {
    Picker picker;
    const Path mnt = picker.services.paths.mnt;
    write_file(mnt / "usb0" / "one.jpg");
    write_file(mnt / "usb0" / "album" / "inner.jpg");
    write_file(mnt / "usb1" / "two.png");

    picker.enter();
    REQUIRE(picker.screen.tabs().size() == 5);
    CHECK(picker.screen.tabs()[2].label == "USB 1");
    CHECK(picker.screen.tabs()[3].label == "USB 2");
    CHECK(picker.screen.tabs()[2].dir == mnt / "usb0");
    CHECK(picker.screen.tabs()[2].count == 1);  // one.jpg; album/ is not counted

    picker.screen.set_tab(picker.context, 2);
    picker.idle();
    REQUIRE(picker.screen.grid().cells().size() == 2);  // album/, one.jpg
    CHECK(picker.folder(0));
    CHECK(picker.screen.browse_dir() == mnt / "usb0");
    picker.update(press(splat::Select));  // album/
    picker.idle();
    CHECK(picker.screen.browse_dir() == mnt / "usb0" / "album");
    CHECK(picker.screen.breadcrumb() == std::vector<std::string>{"album"});

    // The other stick opens at its own top, not inside the first stick's folder.
    picker.screen.set_tab(picker.context, 3);
    picker.idle();
    CHECK(picker.screen.browse_dir() == mnt / "usb1");
    REQUIRE(picker.screen.grid().cells().size() == 1);
    CHECK(picker.caption(0) == "two.png");
    CHECK(picker.screen.breadcrumb().empty());
    CHECK_FALSE(picker.screen.handle_back(picker.context));

    // Coming back to the first stick starts at its top again: a tab switch
    // resets the place.
    picker.screen.set_tab(picker.context, 2);
    picker.idle();
    CHECK(picker.screen.browse_dir() == mnt / "usb0");
    CHECK(picker.screen.breadcrumb().empty());
}
