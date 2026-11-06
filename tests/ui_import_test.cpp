// The picker's splat import: a splat tile with its SPLAT badge and format
// caption, Cross importing one file into the picker's album, Square importing a
// folder's splat files, and a folder with none queueing nothing.
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
#include "gallery/photo_sources.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/photos.h"
#include "ui/screen_stack.h"

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
    REQUIRE(fixture.screen.console_dir() == fixture.data);
    fixture.update(press(splat::Select));  // into photos/
    REQUIRE(fixture.screen.console_dir() == photos);

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

TEST_CASE("Square on a folder queues every splat file below it") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    const Path trip = photos / "trip";
    write_splat_file(trip / "a.spz");
    write_splat_file(trip / "sub" / "c.compressed.ply");
    // One level too deep to be included (the picker stops at depth 3).
    write_splat_file(trip / "sub" / "deeper" / "deepest" / "far.spz");
    write_splat_file(trip / "sub" / "deeper" / "deepest" / "farther" / "too_far.spz");
    write_jpeg(trip / "photo.jpg", 30);

    fixture.open_console();
    fixture.update(press(splat::Select));  // into photos/
    REQUIRE(fixture.screen.console_dir() == photos);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].caption == "trip");
    CHECK(has_hint(fixture.screen.hints(), "Square", "Import splats"));

    fixture.update(press(splat::Secondary));
    REQUIRE(fixture.jobs.last_import().size() == 3);
    CHECK(fixture.jobs.last_import()[0] == trip / "a.spz");
    CHECK(fixture.jobs.last_import()[1] == trip / "sub" / "c.compressed.ply");
    CHECK(fixture.jobs.last_import()[2] == trip / "sub" / "deeper" / "deepest" / "far.spz");
    CHECK(fixture.jobs.last_import_album() == "trip");
    CHECK(has_toast(fixture.toasts, "Queued 3 imports"));
    CHECK(fixture.screen.selection().empty());
}

TEST_CASE("the folder toast counts what the queue actually took") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    const Path trip = photos / "trip";
    write_splat_file(trip / "a.spz");
    write_splat_file(trip / "b.spz");

    // a.spz is already queued (the fake marks it running), so the folder press
    // adds only b.spz: the toast must not claim two.
    const splat::Status queued =
        fixture.jobs.enqueue_import({trip / "a.spz"}, "trip");
    REQUIRE_MESSAGE(queued.ok(), queued.message());
    REQUIRE(fixture.jobs.snapshot().size() == 1);

    fixture.open_console();
    fixture.update(press(splat::Select));  // into photos/
    REQUIRE(fixture.screen.console_dir() == photos);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);

    fixture.update(press(splat::Secondary));
    REQUIRE(fixture.jobs.last_import().size() == 2);
    CHECK(fixture.jobs.snapshot().size() == 2);
    CHECK(has_toast(fixture.toasts, "Queued 1 import"));
    CHECK(!has_toast(fixture.toasts, "Queued 2 imports"));
}

TEST_CASE("a folder import stops at 50 files") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    for (int i = 0; i < 55; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "s%02d.spz", i);
        write_splat_file(photos / "many" / name);
    }

    fixture.open_console();
    fixture.update(press(splat::Select));
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    fixture.update(press(splat::Secondary));
    CHECK(fixture.jobs.last_import().size() == 50);
    CHECK(has_toast(fixture.toasts, "Queued 50 imports"));
}

TEST_CASE("a folder with no splats queues nothing") {
    Fixture fixture;
    const Path photos = fixture.data / "photos";
    write_jpeg(photos / "plain" / "a.jpg", 40);
    write_jpeg(photos / "plain" / "b.jpg", 60);

    fixture.open_console();
    fixture.update(press(splat::Select));
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    fixture.update(press(splat::Secondary));
    CHECK(fixture.jobs.last_import().empty());
    CHECK(has_toast(fixture.toasts, "No splat files in this folder"));
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

    // The listing is newest first, so walk to the tile rather than assuming it.
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

    // list_photos keeps them out unless the caller asks, and flags them when it
    // does (the network tabs in L3 ask).
    const splat::PhotoSource source{"data", "Data", root, false};
    CHECK(splat::list_photos(source).size() == 1);
    const std::vector<splat::PhotoEntry> with_splats = splat::list_photos(source, true);
    REQUIRE(with_splats.size() == 3);
    size_t splats = 0;
    for (const splat::PhotoEntry& entry : with_splats) splats += entry.splat ? 1 : 0;
    CHECK(splats == 2);
}
