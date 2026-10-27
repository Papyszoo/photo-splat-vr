// PhotosScreen: the source tabs (with counts) and their empty states, the
// selection basket behind Cross, the HDR captures that cannot be selected, the
// USB tab that disappears with the stick, and the captures -> inbox copy.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include "common/image.h"
#include "common/path.h"
#include "gallery/photo_sources.h"
#include "jobs/image_io.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screens/photos.h"
#include "ui/screen_stack.h"

using splat::Path;

namespace {

// A tiny real JPEG, so the screen's thumbnail worker decodes a real file.
void write_jpeg(const Path& path, uint8_t red) {
    splat::Image image;
    image.resize(32, 24);
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
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

struct Fixture {
    splat_test::TempDir dir{"ui-photos"};
    Path inbox = dir.path() / "inbox";
    Path mnt = dir.path() / "mnt";
    Path scenes = dir.path() / "scenes";
    Path capture_root = dir.path() / "captures";
    splat::FakeJobService jobs;
    splat::FakeModelService models;
    splat::FakeCaptureService captures{capture_root};
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
        services.paths.scenes = scenes;
        services.paths.state = dir.path() / "state";
        services.jobs = &jobs;
        services.models = &models;
        services.captures = &captures;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        registry.add(splat::ScreenId::ConvertDialog,
                     [] { return std::make_unique<splat::ConvertDialog>(); });
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
};

}  // namespace

TEST_CASE("ui_photos lists the sources and toggles the selection") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    write_jpeg(fixture.inbox / "b.jpg", 60);
    write_jpeg(fixture.inbox / "c.jpg", 110);

    fixture.enter();
    // Always the four sources in this order, plain titles without counts.
    REQUIRE(fixture.screen.tabs().size() == 4);
    CHECK(fixture.screen.tabs()[0].id == "inbox");
    CHECK(fixture.screen.tabs()[0].label == "Inbox");
    CHECK(fixture.screen.tabs()[0].count == 3);
    CHECK(fixture.screen.tabs()[1].id == "console");
    CHECK(fixture.screen.tabs()[2].id == "usb");
    CHECK(fixture.screen.tabs()[2].label == "USB");
    CHECK(fixture.screen.tabs()[3].id == "captures");
    CHECK(fixture.screen.tabs()[3].label == "Captures");
    CHECK(fixture.screen.tabs()[3].count == 4);
    // No L1/R1 prompt in the bar: the badges beside the tabs say it.
    for (const splat::Hint& hint : fixture.screen.hints()) CHECK(hint.button != "L1/R1");
    CHECK(fixture.screen.message().empty());
    CHECK(fixture.screen.selection().empty());

    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 3);
    CHECK_FALSE(fixture.screen.grid().cells()[0].dim);
    CHECK(fixture.screen.album() == "trip");
    CHECK(fixture.screen.title() == "Add photos");

    // The grid order is the gallery's order; the focus starts on the first tile.
    const std::vector<splat::PhotoEntry> listed =
        splat::list_photos(splat::PhotoSource{"inbox", "Inbox", fixture.inbox, false});
    REQUIRE(listed.size() == 3);

    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0] == listed[0].path);
    CHECK(fixture.screen.selected(listed[0].path));
    CHECK(has_hint(fixture.screen.hints(), "Square", "Convert (1)"));
    fixture.idle();
    CHECK(fixture.screen.grid().cells()[0].checked);

    // Cross again clears it: a toggle, not a one-way add.
    fixture.update(press(splat::Select));
    CHECK(fixture.screen.selection().empty());
    CHECK_FALSE(has_hint(fixture.screen.hints(), "Square", "Convert (1)"));
    fixture.idle();
    CHECK_FALSE(fixture.screen.grid().cells()[0].checked);

    // A second photo joins the basket.
    fixture.update(press(splat::Right));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0] == listed[1].path);
    CHECK(has_hint(fixture.screen.hints(), "Square", "Convert (1)"));

    // Drawing paints the tabs, the tiles and the check mark.
    fixture.screen.draw(fixture.context, fixture.canvas, fixture.context.content);
}

TEST_CASE("ui_photos refuses an HDR capture and says why") {
    Fixture fixture;
    fixture.enter();
    fixture.update(press(splat::PagePrev));  // wraps to the last tab
    REQUIRE(fixture.screen.tabs().size() == 4);
    CHECK(fixture.screen.tab() == 3);
    CHECK(fixture.screen.tabs()[3].captures);

    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 4);
    CHECK(fixture.screen.grid().cells()[3].dim);
    CHECK(fixture.screen.grid().cells()[3].sublabel == "HDR");

    for (int step = 0; step < 3; ++step) fixture.update(press(splat::Right));
    REQUIRE(fixture.screen.grid().focus() == 3);
    fixture.update(press(splat::Select));
    CHECK(fixture.screen.selection().empty());
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back().find("HDR") != std::string::npos);

    // The supported capture next to it is selectable.
    fixture.update(press(splat::Left));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0] == fixture.capture_root / "capture_03.jpg");
}

TEST_CASE("ui_photos switches tabs and drops a vanished USB source") {
    Fixture fixture;
    write_jpeg(fixture.mnt / "usb0" / "stick.jpg", 30);
    write_jpeg(fixture.mnt / "usb0" / "second.png", 90);

    fixture.enter();
    REQUIRE(fixture.screen.tabs().size() == 4);
    CHECK(fixture.screen.tabs()[2].id == "usb0");
    CHECK(fixture.screen.tabs()[2].label == "USB");  // one stick: no number
    CHECK(fixture.screen.tabs()[2].count == 2);
    CHECK(fixture.screen.tabs()[2].removable);

    fixture.update(press(splat::PageNext));
    fixture.update(press(splat::PageNext));
    CHECK(fixture.screen.tab() == 2);
    CHECK(fixture.screen.location() == "Photos on the USB drive");
    fixture.idle();
    CHECK(fixture.screen.grid().cells().size() == 2);

    // PagePrev/PageNext wrap at both ends.
    fixture.screen.set_tab(fixture.context, 0);
    fixture.update(press(splat::PagePrev));
    CHECK(fixture.screen.tab() == 3);
    fixture.update(press(splat::PageNext));
    CHECK(fixture.screen.tab() == 0);

    // Two sticks: numbered tabs.
    write_jpeg(fixture.mnt / "usb1" / "third.jpg", 50);
    fixture.screen.refresh(fixture.context);
    REQUIRE(fixture.screen.tabs().size() == 5);
    CHECK(fixture.screen.tabs()[2].label == "USB 1");
    CHECK(fixture.screen.tabs()[3].label == "USB 2");
    std::error_code error;
    std::filesystem::remove_all(fixture.mnt / "usb1", error);

    // Stand on the USB tab, then pull the stick: the tab stays (empty) and the
    // picker falls back to Inbox.
    fixture.screen.refresh(fixture.context);
    fixture.screen.set_tab(fixture.context, 2);
    REQUIRE(fixture.screen.tabs()[2].id == "usb0");
    std::filesystem::remove_all(fixture.mnt / "usb0", error);
    fixture.screen.refresh(fixture.context);
    REQUIRE(fixture.screen.tabs().size() == 4);
    CHECK(fixture.screen.tabs()[2].id == "usb");
    CHECK(fixture.screen.tab() == 0);
    fixture.screen.set_tab(fixture.context, 2);
    CHECK(fixture.screen.message() == "No USB drive found");
    CHECK(fixture.screen.location().empty());
}

TEST_CASE("ui_photos copies the selected captures into the inbox") {
    Fixture fixture;
    fixture.enter();
    fixture.update(press(splat::PagePrev));  // Captures
    fixture.update(splat::UiInput{});

    // Nothing selected: the secondary action explains itself.
    fixture.update(press(splat::Secondary));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "Select captures to copy");
    CHECK(fixture.captures.state() == "idle");

    fixture.update(press(splat::Select));
    fixture.update(press(splat::Right));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 2);
    CHECK(has_hint(fixture.screen.hints(), "Square", "Copy to Inbox"));

    fixture.update(press(splat::Secondary));
    CHECK(fixture.captures.state() == "copied 2");
    CHECK(fixture.screen.selection().empty());
    CHECK(fixture.screen.tab() == 0);
    CHECK(fixture.screen.tabs()[0].id == "inbox");
    CHECK(fixture.toasts.back() == "Copied 2 capture(s) to the inbox");
}

TEST_CASE("ui_photos shows the empty states") {
    Fixture fixture;
    fixture.enter();
    CHECK(fixture.screen.message() == "No photos in the Inbox yet");
    // The line under the tabs says where to upload.
    CHECK(fixture.screen.location() == "Upload photos by FTP to " + fixture.inbox.generic_string());
    fixture.screen.set_tab(fixture.context, 1);
    CHECK(fixture.screen.message() == "No readable storage found");
    fixture.screen.set_tab(fixture.context, 2);
    CHECK(fixture.screen.message() == "No USB drive found");

    std::error_code error;
    std::filesystem::create_directories(fixture.mnt / "usb0", error);
    fixture.screen.refresh(fixture.context);
    REQUIRE(fixture.screen.tabs().size() == 4);
    fixture.screen.set_tab(fixture.context, 2);
    CHECK(fixture.screen.message() == "No photos on this USB drive");

    // Captures without the service: the tab is there and says so.
    fixture.services.captures = nullptr;
    fixture.screen.refresh(fixture.context);
    fixture.screen.set_tab(fixture.context, 3);
    CHECK(fixture.screen.tabs()[3].captures);
    CHECK(fixture.screen.message() == "Captures are not available");
    CHECK(fixture.screen.hints().size() == 1);  // only Circle: nothing to select

    // Under the console's data root, paths read the way FTP shows them.
    Fixture console;
    console.services.paths.data = console.dir.path();
    console.enter();
    CHECK(console.screen.location() == "Upload photos by FTP to /data/inbox");
}

TEST_CASE("ui_photos fills a tile thumbnail from the async cache") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    fixture.enter();
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].thumb == nullptr);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           fixture.screen.grid().cells()[0].thumb == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        fixture.idle();
    }
    REQUIRE(fixture.screen.grid().cells()[0].thumb != nullptr);
    CHECK(fixture.screen.grid().cells()[0].thumb->w == 32);
    CHECK(fixture.screen.grid().cells()[0].thumb->h == 24);
}

TEST_CASE("ui_photos opens the convert dialog with the basket") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    write_jpeg(fixture.inbox / "b.jpg", 60);
    fixture.enter();
    fixture.idle();

    fixture.update(press(splat::Select));
    fixture.update(press(splat::Right));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 2);

    fixture.update(press(splat::Secondary));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::ConvertDialog);
    auto* dialog = dynamic_cast<splat::ConvertDialog*>(fixture.stack.top());
    REQUIRE(dialog != nullptr);
    CHECK(dialog->photos().size() == 2);
    CHECK(dialog->photos()[0] == fixture.screen.selection()[0]);
    // The photos go to the folder the picker was opened from.
    CHECK(dialog->album() == "trip");
}

TEST_CASE("ui_photos square without a selection says what to do") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    fixture.enter();
    fixture.update(press(splat::Secondary));
    CHECK(fixture.stack.empty());
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "Select photos with Cross first");
}

TEST_CASE("ui_photos browses console folders") {
    Fixture fixture;
    const Path data = fixture.dir.path() / "data";
    fixture.services.paths.data = data;
    // The app's own folder lives in /data like on the console, and is hidden.
    fixture.services.paths.inbox = data / "ps5-splat-vr" / "inbox";
    std::filesystem::create_directories(fixture.services.paths.inbox);
    write_jpeg(data / "photos" / "b.jpg", 20);
    write_jpeg(data / "photos" / "A.png", 40);
    std::filesystem::create_directories(data / "photos" / "sub");
    std::filesystem::create_directories(data / "photos" / ".hidden");
    std::ofstream(data / "photos" / "notes.txt") << "not a photo";
    std::filesystem::create_directories(fixture.mnt / "ext0");

    fixture.enter();
    REQUIRE(fixture.screen.tabs().size() == 4);
    CHECK(fixture.screen.tabs()[1].id == "console");
    CHECK(fixture.screen.tabs()[1].console);

    fixture.update(press(splat::PageNext));
    REQUIRE(fixture.screen.tab() == 1);
    CHECK(fixture.screen.console_dir().empty());
    // The tab already says "Console": no line repeating it.
    CHECK(fixture.screen.location().empty());
    fixture.idle();
    // Two places: the internal storage and the extended storage, in words.
    REQUIRE(fixture.screen.grid().cells().size() == 2);
    CHECK(fixture.screen.grid().cells()[0].icon == splat::GridCell::Icon::Folder);
    CHECK(fixture.screen.grid().cells()[0].caption == "Internal storage");
    CHECK(fixture.screen.grid().cells()[1].caption == "Extended storage");
    CHECK(has_hint(fixture.screen.hints(), "Cross", "Open"));
    CHECK(has_hint(fixture.screen.hints(), "Circle", "Back"));

    fixture.update(press(splat::Select));  // into the internal storage
    CHECK(fixture.screen.console_dir() == data);
    CHECK(has_hint(fixture.screen.hints(), "Circle", "Back"));  // a source root
    fixture.idle();
    // The app's folder is not offered.
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].caption == "photos");
    fixture.update(press(splat::Select));  // into photos/
    CHECK(fixture.screen.console_dir() == data / "photos");
    const std::vector<std::string> crumbs = fixture.screen.breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0] == "Internal storage");
    CHECK(crumbs[1] == "photos");
    CHECK(fixture.screen.location().find(data.string()) == std::string::npos);  // never a raw path
    CHECK(has_hint(fixture.screen.hints(), "Circle", "Up"));
    fixture.idle();
    // Folders first, then the photos by name; hidden folders and other files
    // are not shown.
    const std::vector<splat::GridCell>& cells = fixture.screen.grid().cells();
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].caption == "sub");
    CHECK(cells[0].icon == splat::GridCell::Icon::Folder);
    CHECK(cells[1].caption == "A.png");
    CHECK(cells[2].caption == "b.jpg");

    // Cross on a photo selects it; the basket survives going up and down.
    fixture.update(press(splat::Right));
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0] == data / "photos" / "A.png");

    // Circle goes up a folder (the screen keeps it), focusing the folder we
    // came from; from a root back to the list of places; then it leaves.
    CHECK(fixture.screen.handle_back(fixture.context));
    CHECK(fixture.screen.console_dir() == data);
    fixture.idle();
    CHECK(fixture.screen.grid().focus() == 0);
    CHECK(fixture.screen.handle_back(fixture.context));
    CHECK(fixture.screen.console_dir().empty());
    CHECK_FALSE(fixture.screen.handle_back(fixture.context));
    CHECK(fixture.screen.selection().size() == 1);

    // Another tab starts its own basket.
    fixture.update(press(splat::PagePrev));
    CHECK(fixture.screen.tab() == 0);
    CHECK(fixture.screen.selection().empty());
    CHECK_FALSE(fixture.screen.handle_back(fixture.context));

    // With only the internal storage the browser starts inside it, and
    // Circle there leaves the picker.
    std::filesystem::remove_all(fixture.mnt / "ext0");
    fixture.screen.refresh(fixture.context);
    fixture.screen.set_tab(fixture.context, 1);
    CHECK(fixture.screen.console_dir() == data);
    CHECK(fixture.screen.location().empty());
    fixture.idle();
    fixture.update(press(splat::Select));  // photos/
    CHECK(fixture.screen.breadcrumb() == std::vector<std::string>{"photos"});
    CHECK(fixture.screen.location() == "photos");
    CHECK(fixture.screen.handle_back(fixture.context));
    CHECK_FALSE(fixture.screen.handle_back(fixture.context));
}

