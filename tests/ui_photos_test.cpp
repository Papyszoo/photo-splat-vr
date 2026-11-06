// PhotosScreen: the source tabs (with counts) and their empty states, the
// selection basket behind Cross, the HDR captures that cannot be selected, the
// USB tab that disappears with the stick, and the captures -> inbox copy.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include "common/exif.h"
#include "common/image.h"
#include "common/path.h"
#include "gallery/photo_sources.h"
#include "jobs/image_io.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/convert_dialog.h"
#include "ui/screens/keyboard.h"
#include "ui/screens/location_edit.h"
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

// The location editor's row for a field (Save is the last row, so the scan can
// stop there).
int row_of(const splat::LocationEditScreen& screen, splat::LocationEditScreen::Field field) {
    for (int row = 0; row < 32; ++row) {
        const splat::LocationEditScreen::Field at = screen.field_at(row);
        if (at == field) return row;
        if (at == splat::LocationEditScreen::Field::Save) break;
    }
    return -1;
}

// Types `text` on the free-text keyboard and presses OK, the way the pad does.
void type_ok(splat::KeyboardScreen& keyboard, splat::UiContext& context, const std::string& text) {
    for (const char c : text) {
        int index = -1;
        for (int i = 0; i < static_cast<int>(keyboard.keys().size()); ++i) {
            const splat::KeyboardKey& key = keyboard.keys()[static_cast<std::size_t>(i)];
            if (key.kind == splat::KeyboardKey::Kind::Character && key.character == c) index = i;
        }
        REQUIRE_MESSAGE(index >= 0, "no key for " << c);
        keyboard.focus_key(index);
        keyboard.update(context, press(splat::Select));
    }
    int ok = -1;
    for (int i = 0; i < static_cast<int>(keyboard.keys().size()); ++i) {
        if (keyboard.keys()[static_cast<std::size_t>(i)].kind == splat::KeyboardKey::Kind::Ok) ok = i;
    }
    REQUIRE(ok >= 0);
    keyboard.focus_key(ok);
    keyboard.update(context, press(splat::Select));
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
    CHECK(fixture.screen.selection()[0].path == listed[0].path);
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
    CHECK(fixture.screen.selection()[0].path == listed[1].path);
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
    CHECK(fixture.screen.selection()[0].path == fixture.capture_root / "capture_03.jpg");
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
    CHECK(dialog->photos()[0] == fixture.screen.selection()[0].path);
    // The photos go to the folder the picker was opened from.
    CHECK(dialog->album() == "trip");
}

TEST_CASE("ui_photos L3/R3 turns a JPEG by rewriting its orientation tag") {
    Fixture fixture;
    write_jpeg(fixture.inbox / "a.jpg", 10);
    const Path photo = fixture.inbox / "a.jpg";
    const std::vector<uint8_t> original = [&photo] {
        std::ifstream in(photo, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }();
    fixture.enter();
    fixture.idle();

    // The same two keys as the Scenes shelf, and Square is not the turn.
    CHECK(has_hint(fixture.screen.hints(), "L3", "Rotate left"));
    CHECK(has_hint(fixture.screen.hints(), "R3", "Rotate right"));
    CHECK_FALSE(has_hint(fixture.screen.hints(), "Square", "Rotate 90\xC2\xB0"));

    // A JPEG can be rewritten: the source file's Orientation tag changes and no
    // in-memory turn is kept, so a conversion cannot apply the turn twice.
    fixture.update(press(splat::RotateRight));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "Rotated a right (file updated)");
    CHECK(fixture.screen.focused_quarters() == 0);
    std::ifstream in(photo, std::ios::binary);
    const std::vector<uint8_t> turned((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    CHECK(turned != original);
    CHECK(turned.size() > original.size());  // a tag was added, no pixel re-encode
    const auto exif = splat::parse_exif(turned.data(), turned.size());
    REQUIRE_MESSAGE(exif.ok(), exif.message());
    CHECK(exif.value().orientation.value_or(1) == 6);   // 90 clockwise
    // The image bytes after the APP1 segment are untouched: the original JPEG's
    // SOI..EOI payload still follows it.
    CHECK(std::search(turned.begin(), turned.end(), original.begin() + 2, original.end()) !=
          turned.end());

    // Left once restores the original orientation.
    fixture.update(press(splat::RotateLeft));
    CHECK(fixture.toasts.back() == "Rotated a left (file updated)");
    const std::vector<uint8_t> restored = [&photo] {
        std::ifstream back(photo, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(back)),
                                    std::istreambuf_iterator<char>());
    }();
    const auto restored_exif = splat::parse_exif(restored.data(), restored.size());
    REQUIRE_MESSAGE(restored_exif.ok(), restored_exif.message());
    CHECK(restored_exif.value().orientation.value_or(1) == 1);

    // Four right presses cycle 6, 3, 8, 1: upright again, still no saved turn.
    for (int i = 0; i < 4; ++i) fixture.update(press(splat::RotateRight));
    CHECK(fixture.toasts.back() == "Rotated a right (file updated)");
    CHECK(fixture.screen.focused_quarters() == 0);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].turn_quarters == 0);

    // The basket carries 0 for a rewritten file: the tag is the turn.
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0].rotate_quarters == 0);
    CHECK(has_hint(fixture.screen.hints(), "Square", "Convert (1)"));
    fixture.update(press(splat::Secondary));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::ConvertDialog);
    auto* dialog = dynamic_cast<splat::ConvertDialog*>(fixture.stack.top());
    REQUIRE(dialog != nullptr);
    REQUIRE(dialog->picks().size() == 1);
    CHECK(dialog->picks()[0].rotate_quarters == 0);
}

TEST_CASE("ui_photos keeps a turn for a file it cannot rewrite, across a relaunch") {
    Fixture fixture;
    // A PNG has no Orientation tag writer here: the source is left alone and
    // the turn is stored in <state>/photo-turns.json.
    std::vector<uint8_t> rgba(8 * 4 * 4, 120);
    const auto encoded = splat::encode_png(rgba.data(), 8, 4);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    std::filesystem::create_directories(fixture.inbox);
    {
        std::ofstream out(fixture.inbox / "a.png", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(encoded.value().data()),
                  static_cast<std::streamsize>(encoded.value().size()));
    }
    const std::vector<uint8_t> original = [&fixture] {
        std::ifstream in(fixture.inbox / "a.png", std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }();
    fixture.enter();
    fixture.idle();

    fixture.update(press(splat::RotateRight));
    REQUIRE_FALSE(fixture.toasts.empty());
    CHECK(fixture.toasts.back() == "Rotated a right; the file itself was not changed");
    CHECK(fixture.screen.focused_quarters() == 1);
    fixture.idle();
    REQUIRE(fixture.screen.grid().cells().size() == 1);
    CHECK(fixture.screen.grid().cells()[0].turn_quarters == 1);
    // The source bytes are untouched.
    std::ifstream in(fixture.inbox / "a.png", std::ios::binary);
    const std::vector<uint8_t> after((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    CHECK(after == original);
    // And the saved turn goes into the basket, which is what the conversion
    // applies (one quarter, not two).
    fixture.update(press(splat::Select));
    REQUIRE(fixture.screen.selection().size() == 1);
    CHECK(fixture.screen.selection()[0].rotate_quarters == 1);

    // A relaunch: a fresh screen over the same state directory reads the turn
    // back from disk. The drawn turn and the basket both follow.
    splat::PhotosScreen relaunched;
    splat::UiContext context{fixture.services, fixture.theme, fixture.stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    splat::ScreenArgs args;
    args.a = "trip";
    relaunched.on_enter(context, args);
    CHECK(relaunched.focused_quarters() == 1);
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
    CHECK(fixture.screen.selection()[0].path == data / "photos" / "A.png");

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


TEST_CASE("ui_photos the last tab adds a network location") {
    Fixture fixture;
    splat::FakeLocationService locations;
    fixture.services.locations = &locations;
    fixture.registry.add(splat::ScreenId::Photos,
                         [] { return std::make_unique<splat::PhotosScreen>(); });
    fixture.registry.add(splat::ScreenId::LocationEdit,
                         [] { return std::make_unique<splat::LocationEditScreen>(); });
    fixture.registry.add(splat::ScreenId::Keyboard,
                         [] { return std::make_unique<splat::KeyboardScreen>(); });

    // The screen lives in the stack so the editor's return is a real reveal.
    splat::ScreenArgs args;
    args.a = "trip";
    REQUIRE(fixture.stack.push(splat::ScreenId::Photos, args));
    auto* photos = static_cast<splat::PhotosScreen*>(fixture.stack.top());
    photos->update(fixture.context, splat::UiInput{});

    // Inbox, Console, USB, Captures, the photo location, then the add tab.
    std::vector<std::string> ids;
    for (const splat::PhotoTab& tab : photos->tabs()) ids.push_back(tab.id);
    REQUIRE(ids.size() == 6);
    CHECK(ids[0] == "inbox");
    CHECK(ids[1] == "console");
    CHECK(ids[2] == "usb");
    CHECK(ids[3] == "captures");
    CHECK(ids[4] == "smb0");
    CHECK(ids[5] == "add_location");
    CHECK(photos->tabs()[5].title == std::string(splat::PhotosScreen::kAddLocationLabel));
    CHECK(photos->tabs()[5].add_location);

    // The tab lists nothing and says what it does; Cross is the action.
    photos->set_tab(fixture.context, 5);
    CHECK(photos->message() == splat::PhotosScreen::kAddLocationHint);
    CHECK(photos->grid().cells().empty());
    const std::vector<splat::Hint> hints = photos->hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "Add network location");

    // Cross pushes the editor with an empty id (a new location).
    photos->update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::LocationEdit);
    auto* edit = static_cast<splat::LocationEditScreen*>(fixture.stack.top());
    CHECK(edit->title() == "Add location");
    CHECK(edit->draft().id.empty());
    // The picker's own add starts as a photo source: that is the row this
    // screen exists for, and leaving it off is the bug this test drives out.
    CHECK(edit->draft().photo_source);
    CHECK(edit->list().rows()[static_cast<std::size_t>(
                 row_of(*edit, splat::LocationEditScreen::Field::PhotoSource))]
                 .right == "On");

    // Type Host and Share on the panel keyboard, then Cross on Save: nothing
    // else is touched, so the saved row is what the defaults say.
    edit->activate(fixture.context, row_of(*edit, splat::LocationEditScreen::Field::Host));
    auto* keyboard = static_cast<splat::KeyboardScreen*>(fixture.stack.top());
    REQUIRE(keyboard != nullptr);
    type_ok(*keyboard, fixture.context, "nas2.local");
    CHECK(edit->draft().host == "nas2.local");
    edit->activate(fixture.context, row_of(*edit, splat::LocationEditScreen::Field::Share));
    keyboard = static_cast<splat::KeyboardScreen*>(fixture.stack.top());
    REQUIRE(keyboard != nullptr);
    type_ok(*keyboard, fixture.context, "family");
    CHECK(edit->draft().share == "family");
    edit->activate(fixture.context, row_of(*edit, splat::LocationEditScreen::Field::Save));
    // Save popped the editor, which reveals the picker: on_enter -> refresh.
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::Photos);
    const std::vector<splat::LocationView> listed = locations.list();
    REQUIRE(listed.size() == 3);
    const splat::LocationView& added_row = listed.back();
    CHECK(added_row.host == "nas2.local");
    CHECK(added_row.share == "family");
    CHECK(added_row.photo_source);

    photos = static_cast<splat::PhotosScreen*>(fixture.stack.top());
    int added = -1;
    for (std::size_t index = 0; index < photos->tabs().size(); ++index) {
        if (photos->tabs()[index].id == added_row.id) added = static_cast<int>(index);
    }
    CHECK_MESSAGE(added >= 0, "saving from the add tab left no tab for the new share");
    if (added >= 0) CHECK(photos->tabs()[static_cast<std::size_t>(added)].title == "nas2.local/family");
    // Still one terminal add tab, now after the new source.
    CHECK(photos->tabs().back().add_location);
}

TEST_CASE("ui_photos the tab strip stays inside the content rect and follows the focus") {
    Fixture fixture;
    splat::FakeLocationService locations;
    for (int index = 0; index < 6; ++index) {
        splat::LocationView draft;
        draft.name = "share" + std::to_string(index);
        draft.host = "nas" + std::to_string(index) + ".local";
        draft.share = "Photos";
        draft.photo_source = true;
        std::string id;
        REQUIRE(locations.add(draft, &id).ok());
    }
    fixture.services.locations = &locations;
    fixture.enter();
    REQUIRE(fixture.screen.tabs().size() >= 10);

    // A narrow one keeps every rect inside the content rect and always draws the
    // focused tab, scrolling by whole tabs.
    const splat::Rect narrow{16, 78, 420, 586};
    fixture.screen.set_tab(fixture.context, static_cast<int>(fixture.screen.tabs().size()) - 1);
    const std::vector<splat::Rect> rects = fixture.screen.tab_rects(narrow, fixture.theme);
    const int first = fixture.screen.tab_first(narrow, fixture.theme);
    REQUIRE_FALSE(rects.empty());
    CHECK(first > 0);
    CHECK(first <= fixture.screen.tab());
    CHECK(fixture.screen.tab() < first + static_cast<int>(rects.size()));
    for (const splat::Rect& rect : rects) {
        CHECK(rect.x >= narrow.x);
        CHECK(rect.right() <= narrow.right());
        CHECK(rect.h > 0);
    }
    // Drawing the same strip is what the screen does every frame.
    fixture.screen.draw(fixture.context, fixture.canvas, narrow);

    // The left end starts at tab 0 and, with this many tabs, still overflows the
    // wide strip by whole tabs (never by a clipped one).
    fixture.screen.set_tab(fixture.context, 0);
    CHECK(fixture.screen.tab_first(fixture.context.content, fixture.theme) == 0);
    const std::vector<splat::Rect> from_zero =
        fixture.screen.tab_rects(fixture.context.content, fixture.theme);
    CHECK_FALSE(from_zero.empty());
    CHECK(from_zero.size() < fixture.screen.tabs().size());
    for (const splat::Rect& rect : from_zero) {
        CHECK(rect.x >= fixture.context.content.x);
        CHECK(rect.right() <= fixture.context.content.right());
    }
}

TEST_CASE("ui_photos every tab fits on a wide strip") {
    Fixture fixture;
    fixture.enter();
    REQUIRE(fixture.screen.tabs().size() == 4);
    const std::vector<splat::Rect> rects =
        fixture.screen.tab_rects(fixture.context.content, fixture.theme);
    REQUIRE(rects.size() == fixture.screen.tabs().size());
    CHECK(fixture.screen.tab_first(fixture.context.content, fixture.theme) == 0);
    for (const splat::Rect& rect : rects) {
        CHECK(rect.x >= fixture.context.content.x);
        CHECK(rect.right() <= fixture.context.content.right());
    }
}
