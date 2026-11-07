// The picker's splat import: a splat tile with its SPLAT badge and format
// caption, and Cross importing one file into the picker's album from any source
// (Console, USB, Inbox, a network folder). Files are picked one by one as in a
// file browser; Square on a folder tile does nothing (0.6.0, user decision
// 2026-11-07).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/photo_sources.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "scene/ply.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/screens/photos.h"
#include "ui/screen_stack.h"
#include "vfs/io.h"

using splat::Path;

namespace {

// A file the picker only lists: nothing in this test reads its bytes. The
// importer has its own tests (tests/splat_import_test.cpp).
void write_splat_file(const Path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "HTTP/1.0 200 OK\n";  // 16 bytes, not a real splat: never parsed here
    REQUIRE(static_cast<bool>(out));
}

void write_jpeg(const Path& path, uint8_t red) {
    splat::Image image;
    image.resize(16, 12);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(red + x);
            pixel[1] = static_cast<uint8_t>(y * 8);
            pixel[2] = 96;
        }
    }
    const auto bytes = splat::encode_jpeg(image, 85);
    REQUIRE_MESSAGE(bytes.ok(), bytes.message());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.value().data()),
              static_cast<std::streamsize>(bytes.value().size()));
    REQUIRE(static_cast<bool>(out));
}

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

bool has_hint(const std::vector<splat::Hint>& hints, const std::string& button,
              const std::string& label) {
    for (const splat::Hint& hint : hints) {
        if (hint.button == button && hint.label == label) return true;
    }
    return false;
}

bool has_toast(const std::vector<std::string>& toasts, const std::string& text) {
    for (const std::string& toast : toasts) {
        if (toast == text) return true;
    }
    return false;
}

struct Fixture {
    splat_test::TempDir dir{"ui-import"};
    Path data = dir.path() / "data";
    Path inbox = data / "ps5-splat-vr" / "inbox";
    Path mnt = dir.path() / "mnt";
    Path scenes = dir.path() / "scenes";
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::FakeCaptureService captures{dir.path() / "captures"};
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::PhotosScreen screen;
    splat::UiCanvas canvas{1280, 720};
    std::vector<std::string> toasts;

    Fixture() {
        services.paths.inbox = inbox;
        services.paths.mnt = mnt;
        services.paths.data = data;
        services.paths.scenes = scenes;
        services.paths.state = dir.path() / "state";
        services.jobs = &jobs;
        services.models = &models;
        services.captures = &captures;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        stack.set_context(&context);
        std::error_code error;
        std::filesystem::create_directories(inbox, error);
    }

    void enter() {
        splat::ScreenArgs args;
        args.a = "trip";
        screen.on_enter(context, args);
    }

    void update(const splat::UiInput& input) { screen.update(context, input); }
    void idle() { screen.update(context, splat::UiInput{}); }

    // Opens the Console tab and walks down to `dir` (the one storage root is
    // entered automatically by the screen).
    void open_console() {
        enter();
        update(press(splat::PageNext));
        REQUIRE(screen.tab() == 1);
    }
};

}  // namespace

TEST_CASE("a splat tile shows its badge and Cross imports it") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    write_jpeg(photos / "a.jpg", 20);
    write_splat_file(photos / "robot.spz");

    fixture.open_console();
    REQUIRE(fixture.screen.browse_dir() == fixture.data);
    fixture.update(press(splat::Select));  // into photos/
    REQUIRE(fixture.screen.browse_dir() == photos);

    fixture.idle();
    const std::vector<splat::GridCell>& cells = fixture.screen.grid().cells();
    REQUIRE(cells.size() == 2);
    // Photos first, then splat files.
    CHECK(cells[0].caption == "a.jpg");
    CHECK(cells[1].caption == "robot.spz");
    CHECK(cells[1].sublabel == "SPLAT \xC2\xB7 spz");
    CHECK(cells[1].badge == "SPLAT");
    CHECK(cells[1].thumb == nullptr);
    CHECK_FALSE(cells[1].dim);

    // Focus the splat tile: the hints say Import, not Select or Rotate.
    fixture.update(press(splat::Right));
    CHECK(has_hint(fixture.screen.hints(), "Cross", "Import"));
    CHECK_FALSE(has_hint(fixture.screen.hints(), "Cross", "Select"));

    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    CHECK(fixture.jobs.last_import()[0] == photos / "robot.spz");
    CHECK(fixture.jobs.last_import_album() == "trip");
    CHECK(has_toast(fixture.toasts, "Importing robot.spz"));
    // Importing is not selecting: the photo basket stays empty.
    CHECK(fixture.screen.selection().empty());

    // And the queue row the fake service built is an import.
    REQUIRE(fixture.jobs.snapshot().size() == 1);
    CHECK(fixture.jobs.snapshot()[0].name == "robot.spz");
    CHECK(fixture.jobs.snapshot()[0].backend == "import");
    CHECK(fixture.jobs.snapshot()[0].album == "trip");
}

TEST_CASE("Square on a folder tile imports nothing and offers nothing") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    const Path trip = photos / "trip";
    write_splat_file(trip / "a.spz");
    write_splat_file(trip / "sub" / "c.compressed.ply");
    write_jpeg(trip / "photo.jpg", 30);

    fixture.open_console();
    fixture.update(press(splat::Select));  // into photos/
    REQUIRE(fixture.screen.browse_dir() == photos);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].caption == "trip");
    // No folder action in the hint bar: a folder is opened, files are picked.
    CHECK(has_hint(fixture.screen.hints(), "Cross", "Open"));
    for (const splat::Hint& hint : fixture.screen.hints()) {
        CHECK(hint.button != "Square");
        CHECK(hint.label.find("Import") == std::string::npos);
    }

    fixture.update(press(splat::Secondary));
    CHECK(fixture.jobs.last_import().empty());
    CHECK(fixture.jobs.snapshot().empty());
    CHECK(fixture.toasts.empty());
    CHECK(fixture.screen.selection().empty());
    CHECK(fixture.screen.browse_dir() == photos);  // Square did not enter it either
}

TEST_CASE("splat files are imported one by one with Cross, each on its own press") {
    Fixture fixture;
    const Path export_dir = fixture.data / "photos" / "export";
    write_splat_file(export_dir / "a.spz");
    write_splat_file(export_dir / "b.splat");
    write_splat_file(export_dir / "sub" / "c.spz");  // one level down: not listed here

    fixture.open_console();
    fixture.update(press(splat::Select));  // photos/
    fixture.idle();
    fixture.update(press(splat::Select));  // export/
    REQUIRE(fixture.screen.browse_dir() == export_dir);
    fixture.idle();
    // The sub-folder, then the two splat files of this folder only.
    REQUIRE(fixture.screen.grid().cells().size() == 3);
    CHECK(fixture.screen.grid().cells()[0].caption == "sub");

    fixture.update(press(splat::Right));  // a.spz
    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    CHECK(fixture.jobs.last_import()[0] == export_dir / "a.spz");
    CHECK(fixture.jobs.snapshot().size() == 1);

    fixture.update(press(splat::Right));  // b.splat
    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    CHECK(fixture.jobs.last_import()[0] == export_dir / "b.splat");
    CHECK(fixture.jobs.snapshot().size() == 2);
    CHECK(has_toast(fixture.toasts, "Importing a.spz"));
    CHECK(has_toast(fixture.toasts, "Importing b.splat"));
    CHECK(fixture.screen.selection().empty());
}

TEST_CASE("a splat file on a USB stick imports from its folder") {
    Fixture fixture;
    write_jpeg(fixture.mnt / "usb0" / "stick.jpg", 10);
    write_splat_file(fixture.mnt / "usb0" / "export" / "robot.spz");

    fixture.enter();
    REQUIRE(fixture.screen.tabs().size() == 4);
    REQUIRE(fixture.screen.tabs()[2].id == "usb0");
    fixture.screen.set_tab(fixture.context, 2);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 2);  // export/, stick.jpg
    CHECK(fixture.screen.grid().cells()[0].caption == "export");

    fixture.update(press(splat::Select));  // export/
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].badge == "SPLAT");
    CHECK(has_hint(fixture.screen.hints(), "Cross", "Import"));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    CHECK(fixture.jobs.last_import()[0] == fixture.mnt / "usb0" / "export" / "robot.spz");
    CHECK(fixture.jobs.last_import_album() == "trip");
    CHECK(has_toast(fixture.toasts, "Importing robot.spz"));
}

TEST_CASE("a splat file in a network folder imports through the VFS, without any listing") {
    Fixture fixture;
    splat::FakeLocationService locations;
    fixture.services.locations = &locations;
    auto share = std::make_shared<splat_test::TreeFileSystem>("smb0");
    splat::vfs::file_systems().add(share);
    struct Unmount {
        ~Unmount() { splat::vfs::file_systems().remove("smb0"); }
    } unmount;

    // A real scene, exported to a PLY, is what the share serves for robot.ply.
    splat_test::TempDir work("ui-import-vfs");
    const splat::SplatScene scene = splat_test::make_test_scene(4);
    REQUIRE(splat::export_ply(work.path() / "robot.ply", scene).ok());
    std::vector<uint8_t> bytes;
    {
        std::ifstream in(work.path() / "robot.ply", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    REQUIRE_FALSE(bytes.empty());
    share->files["scenes/export/robot.ply"] = bytes;
    share->files["scenes/export/deeper/far.spz"] = {3};
    share->files["scenes/photo.jpg"] = {4};

    fixture.enter();
    int tab = -1;
    for (std::size_t index = 0; index < fixture.screen.tabs().size(); ++index) {
        if (fixture.screen.tabs()[index].id == "smb0") tab = static_cast<int>(index);
    }
    REQUIRE(tab >= 0);
    const auto wait_cells = [&fixture](std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        while (fixture.screen.grid().cells().size() != count &&
               std::chrono::steady_clock::now() < deadline) {
            fixture.idle();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(fixture.screen.grid().cells().size() == count);
    };
    fixture.screen.set_tab(fixture.context, tab);
    wait_cells(2);  // export/, photo.jpg
    CHECK(fixture.screen.grid().cells()[0].caption == "export");
    fixture.update(press(splat::Select));  // export/
    wait_cells(2);                         // deeper/, robot.ply
    CHECK(fixture.screen.grid().cells()[1].caption == "robot.ply");
    CHECK(fixture.screen.grid().cells()[1].badge == "SPLAT");

    fixture.update(press(splat::Right));
    CHECK(has_hint(fixture.screen.hints(), "Cross", "Import"));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    // The job gets the location path itself; the destination is the local library.
    const Path remote = fixture.jobs.last_import()[0];
    CHECK(remote == Path("smb0:/scenes/export/robot.ply"));
    CHECK(fixture.jobs.last_import_album() == "trip");
    CHECK(fixture.jobs.last_output_root().empty());
    CHECK(has_toast(fixture.toasts, "Importing robot.ply"));
    // Two listings for two visits, none for the import: nothing walked the share.
    CHECK(share->list_calls() == std::vector<std::string>{"scenes", "scenes/export"});

    // And the importer behind that path reads the file through the VFS.
    auto opened = splat::JobQueue::open(work.path() / "state" / "jobs.json");
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    splat::JobQueue queue = opened.take();
    const auto id = queue.enqueue_import(remote, std::string());
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::GpuPhaseGuard guard;
    splat::JobRunnerConfig config;
    config.output_root = work.path() / "scenes-local";
    splat::JobRunner runner(queue, guard, nullptr, config);
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK_MESSAGE(outcome.value()->state == splat::JobState::Done, outcome.value()->message);
    const auto loaded = splat::read_scene_folder(outcome.value()->scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 4);
    CHECK(loaded.value().meta.backend == "import");
    CHECK(outcome.value()->scene_folder.filename() == "robot");
}

TEST_CASE("the Inbox tab lists splat files too, and imports them") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    write_splat_file(fixture.inbox / "uploaded.splat");

    fixture.enter();
    fixture.idle();
    const std::vector<splat::GridCell>& cells = fixture.screen.grid().cells();
    REQUIRE(cells.size() == 2);
    bool found = false;
    int splat_index = -1;
    for (size_t index = 0; index < cells.size(); ++index) {
        if (cells[index].caption != "uploaded.splat") continue;
        found = true;
        splat_index = static_cast<int>(index);
        CHECK(cells[index].badge == "SPLAT");
        CHECK(cells[index].sublabel == "SPLAT \xC2\xB7 splat");
    }
    CHECK(found);
    REQUIRE(splat_index >= 0);

    // Walk to the tile rather than assuming where the name order puts it.
    for (int step = 0; step < splat_index; ++step) fixture.update(press(splat::Right));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.jobs.last_import().size() == 1);
    CHECK(fixture.jobs.last_import()[0] == fixture.inbox / "uploaded.splat");
}

TEST_CASE("list_folder sorts folders, photos, then splat files") {
    splat_test::TempDir dir("list-folder-splats");
    const Path root = dir.path();
    write_splat_file(root / "zz.spz");
    write_splat_file(root / "aa.splat");
    write_jpeg(root / "mm.jpg", 10);
    std::filesystem::create_directories(root / "nested");

    const std::vector<splat::FolderEntry> entries = splat::list_folder(root);
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].folder);
    CHECK(entries[0].name == "nested");
    CHECK(entries[1].name == "mm.jpg");
    CHECK_FALSE(entries[1].splat);
    CHECK(entries[2].name == "aa.splat");
    CHECK(entries[2].splat);
    CHECK(entries[2].source_count == 0);
    CHECK(entries[3].name == "zz.spz");
    CHECK(entries[3].splat);
}
