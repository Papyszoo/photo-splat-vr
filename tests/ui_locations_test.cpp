// The Locations screen and the free-text keyboard (L3-C): the rows of a fake
// service, Cross opening the library as a scene root, Square's delete card, a
// failed mount showing offline, and the typed value reaching on_text (masked or
// not) with exactly one call.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "common/image.h"
#include "common/path.h"
#include "gallery/gallery.h"
#include "gallery/visibility.h"
#include "jobs/image_io.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/location_service.h"
#include "ui/screens/keyboard.h"
#include "ui/screens/location_edit.h"
#include "ui/screens/locations.h"
#include "ui/screens/photos.h"
#include "ui/screens/scenes.h"
#include "ui/screen_stack.h"
#include "vfs/io.h"
#include "vfs/location.h"

namespace {

class CapturingScreen : public splat::Screen {
public:
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override { last = args; }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }

    splat::ScreenArgs last;
};

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

struct Fixture {
    splat::FakeLocationService locations;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::LocationsScreen* screen = nullptr;
    std::vector<std::string> toasts;
    CapturingScreen* scenes = nullptr;

    Fixture() {
        services.locations = &locations;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        stack.set_context(&context);
        registry.add(splat::ScreenId::Locations, [this] {
            auto created = std::make_unique<splat::LocationsScreen>();
            screen = created.get();
            return std::unique_ptr<splat::Screen>(std::move(created));
        });
        // A share opens as the albums home of its library (ScreenId::Albums).
        registry.add(splat::ScreenId::Albums, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            scenes = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::LocationEdit, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            return screen;
        });
        stack.push(splat::ScreenId::Locations);
    }
};

}  // namespace

TEST_CASE("ui_locations lists the configured shares and an add row") {
    Fixture fixture;
    REQUIRE(fixture.screen != nullptr);
    const std::vector<std::string>& names = fixture.screen->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "nas.local/Photos");
    CHECK(names[1] == "192.168.1.20/Scenes");
    CHECK(names[2] == splat::LocationsScreen::kAddLabel);
    REQUIRE(fixture.screen->list().rows().size() == 3);
    CHECK(fixture.screen->list().rows()[1].right == "offline: connect timed out");
    CHECK(fixture.screen->list().rows()[2].add);
}

TEST_CASE("ui_locations Cross opens the share as a library root") {
    Fixture fixture;
    fixture.screen->set_focus(0);
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.scenes != nullptr);
    CHECK(fixture.scenes->last.b == "smb0:/scenes");
    CHECK(fixture.scenes->last.a.empty());
}

TEST_CASE("ui_locations Square asks before removing a share") {
    Fixture fixture;
    fixture.screen->set_focus(0);
    fixture.stack.update(fixture.context, press(splat::Secondary));
    CHECK(fixture.screen->confirm().is_open());
    CHECK(fixture.screen->confirm().detail().find("files on the share stay") !=
          std::string::npos);
    CHECK(has_hint(fixture.screen->hints(), "Cross", "Choose"));
}

TEST_CASE("ui_locations shows a failed mount as offline") {
    Fixture fixture;
    splat::LocationView broken;
    broken.protocol = "smb";
    broken.host = "unreachable";
    broken.share = "Share";
    broken.photo_source = false;
    std::string id;
    REQUIRE(fixture.locations.add(broken, &id).ok());
    REQUIRE(fixture.locations.mount(id).ok());
    fixture.stack.update(fixture.context, press(0));

    bool found = false;
    for (const splat::ListRow& row : fixture.screen->list().rows()) {
        if (row.right == "offline: connect timed out") found = true;
    }
    CHECK(found);
}

TEST_CASE("keyboard free text types a URL and calls on_text once") {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);

    std::string delivered;
    int calls = 0;
    registry.add(splat::ScreenId::Settings, [&] {
        return std::unique_ptr<splat::Screen>(new CapturingScreen());
    });
    registry.add(splat::ScreenId::Keyboard, [] {
        return std::unique_ptr<splat::Screen>(new splat::KeyboardScreen());
    });
    stack.push(splat::ScreenId::Settings);
    splat::ScreenArgs args;
    args.a = "free-text";
    args.b = "";
    args.masked = false;
    args.max_length = 64;
    args.on_text = [&](const std::string& text) {
        delivered = text;
        ++calls;
    };
    stack.push(splat::ScreenId::Keyboard, args);

    splat::KeyboardScreen* keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    CHECK(keyboard->free_text());

    const std::string typed = "p@ss:w/1.a-b_c";
    for (const char c : typed) {
        int index = -1;
        for (int i = 0; i < static_cast<int>(keyboard->keys().size()); ++i) {
            const splat::KeyboardKey& key = keyboard->keys()[static_cast<std::size_t>(i)];
            if (key.kind == splat::KeyboardKey::Kind::Character && key.character == c) index = i;
        }
        REQUIRE_MESSAGE(index >= 0, "no key for " << c);
        keyboard->focus_key(index);
        keyboard->update(context, press(splat::Select));
    }
    CHECK(keyboard->text() == typed);
    CHECK(keyboard->valid());

    int ok = -1;
    for (int i = 0; i < static_cast<int>(keyboard->keys().size()); ++i) {
        if (keyboard->keys()[static_cast<std::size_t>(i)].kind ==
            splat::KeyboardKey::Kind::Ok) {
            ok = i;
        }
    }
    REQUIRE(ok >= 0);
    keyboard->focus_key(ok);
    keyboard->update(context, press(splat::Select));

    CHECK(calls == 1);
    CHECK(delivered == typed);
    CHECK(stack.find(splat::ScreenId::Keyboard) < 0);  // popped itself
}

TEST_CASE("keyboard free text honours the caller's max_length, not 64") {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);

    splat::KeyboardScreen keyboard;
    splat::ScreenArgs args;
    args.a = "free-text";
    args.max_length = 100;  // a share path, not an album name
    keyboard.on_enter(context, args);
    for (const char c : std::string(70, 'x')) {
        int index = -1;
        for (int i = 0; i < static_cast<int>(keyboard.keys().size()); ++i) {
            const splat::KeyboardKey& key = keyboard.keys()[static_cast<std::size_t>(i)];
            if (key.kind == splat::KeyboardKey::Kind::Character && key.character == c) index = i;
        }
        REQUIRE(index >= 0);
        keyboard.focus_key(index);
        keyboard.update(context, press(splat::Select));
    }
    CHECK(keyboard.text().size() == 70u);

    // And the caller's ceiling still holds.
    splat::ScreenArgs capped;
    capped.a = "free-text";
    capped.max_length = 10;
    capped.b = std::string(20, 'y');
    keyboard.on_enter(context, capped);
    CHECK(keyboard.text().size() == 10u);
}

TEST_CASE("keyboard free text masked draws one star per character") {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);

    splat::KeyboardScreen keyboard;
    splat::ScreenArgs args;
    args.a = "free-text";
    args.b = "p@ss:w/1";
    args.masked = true;
    keyboard.on_enter(context, args);

    CHECK(keyboard.masked());
    CHECK(keyboard.text() == "p@ss:w/1");
    CHECK(keyboard.drawn_text() == "********");

    splat::ScreenArgs plain;
    plain.a = "free-text";
    plain.b = "p@ss:w/1";
    keyboard.on_enter(context, plain);
    CHECK_FALSE(keyboard.masked());
    CHECK(keyboard.drawn_text() == "p@ss:w/1");
}

namespace {

// One network location as a VFS file system: a flat "scenes" folder with the
// names the test puts in it.
class FlatFakeFileSystem : public splat::vfs::IFileSystem {
public:
    explicit FlatFakeFileSystem(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t) override {
        const auto found = files_.find(relative);
        if (found == files_.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file");
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::success();
    }
    splat::Status list(const std::string&, std::vector<splat::vfs::DirEntry>* entries) override {
        for (const auto& file : files_) {
            splat::vfs::DirEntry entry;
            const std::size_t slash = file.first.find_last_of('/');
            entry.name = slash == std::string::npos ? file.first : file.first.substr(slash + 1);
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        if (relative.empty() || relative == "scenes") {
            out->exists = true;
            out->directory = true;
            return splat::Status::success();
        }
        const auto found = files_.find(relative);
        if (found == files_.end()) return splat::Status::success();
        out->exists = true;
        out->regular = true;
        out->size = found->second.size();
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }
    // What the Locations panel asks every tick (smb-e2e BUG 1's screen half).
    std::string connection_error() const override { return health_; }

    std::map<std::string, std::vector<uint8_t>> files_;
    std::string health_;

private:
    std::string id_;
};

// A share whose connection is gone: every route fails with the adapter's
// message, which is what a listing must show instead of an empty folder.
class OfflineFileSystem : public splat::vfs::IFileSystem {
public:
    explicit OfflineFileSystem(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return false; }

    splat::Result<std::vector<uint8_t>> read(const std::string&, size_t) override {
        return splat::Result<std::vector<uint8_t>>::failure(kError);
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::error(kError);
    }
    splat::Status list(const std::string&, std::vector<splat::vfs::DirEntry>*) override {
        return splat::Status::error(kError);
    }
    splat::Status stat(const std::string&, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        return splat::Status::error(kError);
    }
    splat::Status make_dirs(const std::string&) override {
        return splat::Status::error(kError);
    }
    splat::Status remove_file(const std::string&) override {
        return splat::Status::error(kError);
    }
    splat::Status remove_tree(const std::string&) override {
        return splat::Status::error(kError);
    }

    static constexpr const char* kError = "smb0: connect timed out";

private:
    std::string id_;
};

// One network location that looks like a scene library: "<id>:/scenes" holds one
// scene folder with its splat, so the gallery lists it.
class RemoteSceneFileSystem : public splat::vfs::IFileSystem {
public:
    explicit RemoteSceneFileSystem(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t) override {
        return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::success();
    }
    splat::Status list(const std::string& relative,
                       std::vector<splat::vfs::DirEntry>* entries) override {
        splat::vfs::DirEntry entry;
        if (relative.empty()) {
            entry.name = "scenes";
            entry.directory = true;
        } else if (relative == "scenes") {
            entry.name = "one";
            entry.directory = true;
        } else if (relative == "scenes/one") {
            entry.name = "scene.psplat";
            entry.size = 8;
        } else {
            return splat::Status::success();
        }
        entries->push_back(entry);
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        if (relative.empty() || relative == "scenes" || relative == "scenes/one") {
            out->exists = true;
            out->directory = true;
            return splat::Status::success();
        }
        if (relative == "scenes/one/scene.psplat") {
            out->exists = true;
            out->regular = true;
            out->size = 8;
        }
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }

private:
    std::string id_;
};

}  // namespace

TEST_CASE("a network photo tab lists three photos and one splat") {
    // A passcode is set and nothing is ticked: a location's photos are not part
    // of the visible-folders list at all (stage U1), so the tab lists them.
    splat::Visibility hidden = splat::Visibility::all_visible(splat::VisibilityContext{});
    hidden.enabled = true;
    splat::VisibilityScope hidden_scope(std::move(hidden));

    auto mounted = std::make_shared<FlatFakeFileSystem>("smb0");
    mounted->files_["scenes/a.jpg"] = {1};
    mounted->files_["scenes/b.png"] = {2};
    mounted->files_["scenes/c.webp"] = {3};
    mounted->files_["scenes/d.spz"] = {4};
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;

    splat_test::TempDir dir("ui-location-photos");
    std::error_code error;
    std::filesystem::create_directories(dir.path() / "inbox", error);
    std::filesystem::create_directories(dir.path() / "scenes", error);
    std::filesystem::create_directories(dir.path() / "mnt", error);

    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    services.paths.inbox = dir.path() / "inbox";
    services.paths.scenes = dir.path() / "scenes";
    services.paths.mnt = dir.path() / "mnt";
    services.paths.data = dir.path() / "data";
    services.paths.state = dir.path() / "state";

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);
    registry.add(splat::ScreenId::Photos, [] {
        return std::unique_ptr<splat::Screen>(new splat::PhotosScreen());
    });
    stack.push(splat::ScreenId::Photos);

    splat::PhotosScreen* photos = dynamic_cast<splat::PhotosScreen*>(stack.top());
    REQUIRE(photos != nullptr);

    int tab = -1;
    for (std::size_t index = 0; index < photos->tabs().size(); ++index) {
        if (photos->tabs()[index].id == "smb0") tab = static_cast<int>(index);
    }
    REQUIRE_MESSAGE(tab >= 0, "no photo tab for the location");
    photos->set_tab(context, tab);
    CHECK(photos->tabs()[static_cast<std::size_t>(tab)].dir == splat::Path("smb0:/scenes"));

    // A location is listed on the worker: pump frames until the grid is built
    // (tick() publishes, never blocks).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (photos->grid().cells().size() != 4 && std::chrono::steady_clock::now() < deadline) {
        photos->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::vector<splat::GridCell>& cells = photos->grid().cells();
    REQUIRE(cells.size() == 4);
    int splats = 0;
    for (const splat::GridCell& cell : cells) {
        if (cell.badge == "SPLAT") ++splats;
    }
    CHECK(splats == 1);
}

TEST_CASE("an unreachable photo location shows the share's error, not an empty folder") {
    auto mounted = std::make_shared<OfflineFileSystem>("smb0");
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;

    splat_test::TempDir dir("ui-location-offline-photos");
    std::error_code error;
    std::filesystem::create_directories(dir.path() / "inbox", error);
    std::filesystem::create_directories(dir.path() / "scenes", error);
    std::filesystem::create_directories(dir.path() / "mnt", error);

    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    services.paths.inbox = dir.path() / "inbox";
    services.paths.scenes = dir.path() / "scenes";
    services.paths.mnt = dir.path() / "mnt";
    services.paths.state = dir.path() / "state";

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);
    registry.add(splat::ScreenId::Photos, [] {
        return std::unique_ptr<splat::Screen>(new splat::PhotosScreen());
    });
    stack.push(splat::ScreenId::Photos);
    auto* photos = dynamic_cast<splat::PhotosScreen*>(stack.top());
    REQUIRE(photos != nullptr);

    int tab = -1;
    for (std::size_t index = 0; index < photos->tabs().size(); ++index) {
        if (photos->tabs()[index].id == "smb0") tab = static_cast<int>(index);
    }
    REQUIRE_MESSAGE(tab >= 0, "no photo tab for the location");
    photos->set_tab(context, tab);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (photos->message().empty() && std::chrono::steady_clock::now() < deadline) {
        photos->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(photos->message().find("connect timed out") != std::string::npos);
    CHECK(photos->message().find("No photos") == std::string::npos);
    CHECK(photos->grid().cells().empty());
}

TEST_CASE("an unreachable scene location shows the share's error, not an empty album") {
    auto mounted = std::make_shared<OfflineFileSystem>("smb0");
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;

    splat_test::TempDir dir("ui-location-offline-scenes");
    std::filesystem::create_directories(dir.path() / "scenes");

    splat::UiServices services;
    services.paths.scenes = dir.path() / "scenes";
    std::vector<std::string> log;
    services.log = [&log](const std::string& line) { log.push_back(line); };

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);
    registry.add(splat::ScreenId::Scenes, [] {
        return std::unique_ptr<splat::Screen>(new splat::ScenesScreen());
    });

    splat::ScreenArgs args;
    args.a.clear();
    args.b = "smb0:/scenes";
    REQUIRE(stack.push(splat::ScreenId::Scenes, args));
    auto* scenes = dynamic_cast<splat::ScenesScreen*>(stack.top());
    REQUIRE(scenes != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (scenes->error().empty() && std::chrono::steady_clock::now() < deadline) {
        scenes->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(scenes->error().find("connect timed out") != std::string::npos);
    CHECK(scenes->names().empty());
    REQUIRE_FALSE(log.empty());
    CHECK(log.back().find("ui: scenes: smb0: connect timed out") != std::string::npos);
}

TEST_CASE("a network photo turns in the picker exactly like a local one") {
    // A real JPEG in the fake share, so the tile is a photo the picker can turn
    // (the other fake listings use one-byte placeholders and never decode).
    splat::Image jpeg_image;
    jpeg_image.resize(32, 24);
    for (uint32_t y = 0; y < 24; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            uint8_t* pixel = jpeg_image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(10 + x);
            pixel[1] = static_cast<uint8_t>(y * 8);
            pixel[2] = 96;
        }
    }
    const auto encoded = splat::encode_jpeg(jpeg_image, 85);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());

    auto mounted = std::make_shared<FlatFakeFileSystem>("smb0");
    mounted->files_["scenes/one.jpg"] = encoded.value();
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;

    splat_test::TempDir dir("ui-location-turn");
    std::error_code error;
    std::filesystem::create_directories(dir.path() / "inbox", error);
    std::filesystem::create_directories(dir.path() / "scenes", error);
    std::filesystem::create_directories(dir.path() / "mnt", error);

    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    services.paths.inbox = dir.path() / "inbox";
    services.paths.scenes = dir.path() / "scenes";
    services.paths.mnt = dir.path() / "mnt";
    services.paths.data = dir.path() / "data";
    services.paths.state = dir.path() / "state";

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    std::vector<std::string> toasts;
    context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(std::move(text)); };
    stack.set_context(&context);
    registry.add(splat::ScreenId::Photos, [] {
        return std::unique_ptr<splat::Screen>(new splat::PhotosScreen());
    });
    stack.push(splat::ScreenId::Photos);
    auto* photos = dynamic_cast<splat::PhotosScreen*>(stack.top());
    REQUIRE(photos != nullptr);

    int tab = -1;
    for (std::size_t index = 0; index < photos->tabs().size(); ++index) {
        if (photos->tabs()[index].id == "smb0") tab = static_cast<int>(index);
    }
    REQUIRE_MESSAGE(tab >= 0, "no photo tab for the location");
    photos->set_tab(context, tab);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (photos->grid().cells().size() != 1 && std::chrono::steady_clock::now() < deadline) {
        photos->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(photos->grid().cells().size() == 1);

    // R3 turns the focused network photo: the same file action, toast and
    // drawn turn as an inbox tile. This fake share has no rename, so the
    // source is left alone and the turn is saved on this console.
    photos->update(context, press(splat::RotateRight));
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Rotated one right; the file itself was not changed");
    CHECK(photos->focused_quarters() == 1);
    photos->update(context, splat::UiInput{});
    CHECK(photos->grid().cells()[0].turn_quarters == 1);

    // Cross puts it in the basket with that turn.
    photos->update(context, press(splat::Select));
    REQUIRE(photos->selection().size() == 1);
    CHECK(photos->selection()[0].rotate_quarters == 1);

    // A re-list (the same path as R1/L1 or a refresh) keeps the turn.
    photos->refresh(context);
    const auto relisted = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (photos->grid().cells().size() != 1 && std::chrono::steady_clock::now() < relisted) {
        photos->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(photos->grid().cells().size() == 1);
    CHECK(photos->grid().cells()[0].turn_quarters == 1);
    REQUIRE(photos->selection().size() == 1);
    CHECK(photos->selection()[0].rotate_quarters == 1);
}

TEST_CASE("a network library refuses the scene context menu") {
    auto mounted = std::make_shared<RemoteSceneFileSystem>("smb0");
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;

    splat_test::TempDir dir("ui-remote-scenes");
    std::filesystem::create_directories(dir.path() / "scenes");

    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    services.paths.scenes = dir.path() / "scenes";

    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    std::vector<std::string> toasts;
    context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(std::move(text)); };
    stack.set_context(&context);
    registry.add(splat::ScreenId::Scenes, [] {
        return std::unique_ptr<splat::Screen>(new splat::ScenesScreen());
    });

    splat::ScreenArgs args;
    args.a.clear();              // the location root is the album
    args.b = "smb0:/scenes";     // the terminal root the screen lists
    REQUIRE(stack.push(splat::ScreenId::Scenes, args));

    splat::ScenesScreen* scenes = dynamic_cast<splat::ScenesScreen*>(stack.top());
    REQUIRE(scenes != nullptr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (scenes->names().empty() && std::chrono::steady_clock::now() < deadline) {
        scenes->update(context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(scenes->names().size() == 1);
    CHECK(scenes->names()[0] == "one");
    CHECK(scenes->root() == splat::Path("smb0:/scenes"));

    // Triangle: the folder cannot be moved, renamed or deleted from the console,
    // so the menu never opens and the refusal is what the user reads.
    toasts.clear();
    scenes->update(context, press(splat::Context));
    CHECK_FALSE(scenes->menu().is_open());
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == std::string(splat::kRemoteMutationRefusal));
}

TEST_CASE("a location added while another mount runs is queued, not lost") {
    splat_test::TempDir dir("ui-location-mount-queue");
    splat::Result<splat::vfs::LocationStore> opened =
        splat::vfs::LocationStore::open(dir.path() / "state" / "locations.json");
    REQUIRE(opened.ok());
    splat::vfs::LocationStore store = opened.take();
    splat::vfs::Location first;
    first.host = "first.local";
    first.share = "First";
    REQUIRE(store.add(first).ok());

    std::mutex mutex;
    std::vector<std::string> probed;
    auto locations = std::make_unique<splat::LocationService>(
        std::move(store), splat::vfs::file_systems(), [] {},
        [&mutex, &probed](const splat::vfs::Location& location,
                          splat::vfs::FileSystems&) -> std::string {
            // Slow enough that the add() below lands while this mount runs.
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard<std::mutex> lock(mutex);
            probed.push_back(location.id);
            return "ready";
        });

    splat::LocationView second;
    second.name = "second";
    second.protocol = "smb";
    second.host = "second.local";
    second.share = "Second";
    std::string id;
    REQUIRE(locations->add(second, &id).ok());
    CHECK(id == "smb1");
    CHECK(locations->status(id) == "mounting");  // still queued behind smb0

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (locations->busy() && std::chrono::steady_clock::now() < deadline) {
        locations->tick(1.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    locations->tick(1.0 / 60.0);
    CHECK_FALSE(locations->busy());
    CHECK(locations->status("smb0") == "ready");
    CHECK(locations->status(id) == "ready");
    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(probed.size() == 2);
        CHECK(probed[0] == "smb0");
        CHECK(probed[1] == "smb1");
    }
}

TEST_CASE("an AUTH_SYS id above 2^31 survives the location service and its file") {
    splat_test::TempDir dir("ui-location-uid-range");
    splat::Result<splat::vfs::LocationStore> opened =
        splat::vfs::LocationStore::open(dir.path() / "state" / "locations.json");
    REQUIRE(opened.ok());
    auto locations = std::make_unique<splat::LocationService>(
        opened.take(), splat::vfs::file_systems(), [] {},
        [](const splat::vfs::Location&, splat::vfs::FileSystems&) { return std::string("ready"); });

    splat::LocationView nfs;
    nfs.protocol = "nfs";
    nfs.host = "nas.local";
    nfs.share = "/export";
    // A valid uint32_t AUTH_SYS id that does not fit in an int. Narrowing it
    // would come back negative and the next save would write 0 = root.
    nfs.uid = 4294967294u;
    nfs.gid = 0;
    std::string id;
    REQUIRE(locations->add(nfs, &id).ok());

    const std::vector<splat::LocationView> listed = locations->list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].uid == 4294967294u);
    CHECK(listed[0].gid == 0u);
    CHECK(listed[0].port == 0);

    // The file keeps it too, so a restart does not turn the location into root.
    const splat::Result<std::vector<uint8_t>> bytes =
        splat::vfs::read_file(dir.path() / "state" / "locations.json");
    REQUIRE(bytes.ok());
    const std::string json(bytes.value().begin(), bytes.value().end());
    CHECK(json.find("\"uid\": 4294967294") != std::string::npos);
    CHECK(json.find("\"gid\": 0") != std::string::npos);
}

namespace {

int row_for_field(const splat::LocationEditScreen& screen,
                  splat::LocationEditScreen::Field field) {
    for (int row = 0; row < 32; ++row) {
        const splat::LocationEditScreen::Field at = screen.field_at(row);
        if (at == field) return row;
        if (at == splat::LocationEditScreen::Field::Save) break;  // the last row
    }
    return -1;
}

void type_text(splat::KeyboardScreen& keyboard, splat::UiContext& context,
               const std::string& text) {
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
}

void press_kind(splat::KeyboardScreen& keyboard, splat::UiContext& context,
                splat::KeyboardKey::Kind kind) {
    int index = -1;
    for (int i = 0; i < static_cast<int>(keyboard.keys().size()); ++i) {
        if (keyboard.keys()[static_cast<std::size_t>(i)].kind == kind) index = i;
    }
    REQUIRE(index >= 0);
    keyboard.focus_key(index);
    keyboard.update(context, press(splat::Select));
}

}  // namespace

TEST_CASE("location edit adds a share typed on the panel keyboard") {
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    std::vector<std::string> toasts;
    context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(std::move(text)); };
    stack.set_context(&context);
    registry.add(splat::ScreenId::LocationEdit, [] {
        return std::unique_ptr<splat::Screen>(new splat::LocationEditScreen());
    });
    registry.add(splat::ScreenId::Keyboard, [] {
        return std::unique_ptr<splat::Screen>(new splat::KeyboardScreen());
    });
    REQUIRE(stack.push(splat::ScreenId::LocationEdit, splat::ScreenArgs{}));
    auto* edit = dynamic_cast<splat::LocationEditScreen*>(stack.top());
    REQUIRE(edit != nullptr);

    // Host: a dotted IP has to be typeable, and the keyboard writes the draft.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Host));
    auto* keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    type_text(*keyboard, context, "192.168.1.20");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().host == "192.168.1.20");
    CHECK(stack.top() == edit);  // the keyboard popped itself

    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Share));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    type_text(*keyboard, context, "photos");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().share == "photos");

    // The two roles have a writer on the screen, not only in the store.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::PhotoSource));
    CHECK(edit->draft().photo_source);
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::SplatLibrary));
    CHECK(edit->draft().splat_library);

    toasts.clear();
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Save));
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == "Added smb2");  // a new location is added, not saved
    const std::vector<splat::LocationView> listed = locations.list();
    REQUIRE(listed.size() == 3);
    const splat::LocationView& added = listed.back();
    CHECK(added.id == "smb2");
    CHECK(added.host == "192.168.1.20");
    CHECK(added.share == "photos");
    CHECK(added.photo_source);
    CHECK(added.splat_library);
}

TEST_CASE("location edit Test shows the mount answer when it arrives") {
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    std::vector<std::string> logs;
    services.log = [&logs](const std::string& text) { logs.push_back(text); };
    stack.set_context(&context);

    splat::LocationEditScreen edit;
    splat::ScreenArgs args;
    args.a = "smb0";
    edit.on_enter(context, args);

    edit.activate(context, row_for_field(edit, splat::LocationEditScreen::Field::Test));
    CHECK(edit.status_text() == "Testing \xE2\x80\xA6");

    // The edit screen drives the location service's worker itself; before this
    // the line stayed "Testing …" until the user backed out to the list.
    splat::UiInput input;
    input.dt = 1.0 / 60.0;
    edit.update(context, input);
    CHECK(edit.status_text() == "Ready: SMB nas.local/Photos/scenes");
    REQUIRE_FALSE(logs.empty());
    CHECK(logs.back() == "ui: location smb0: Ready: SMB nas.local/Photos/scenes");
}

TEST_CASE("location edit shows the NFS uid and gid the location holds") {
    splat::FakeLocationService locations;
    splat::LocationView nfs;
    nfs.id = "smb1";  // an existing row, re-typed as an NFS export
    nfs.name = "nas/export";
    nfs.protocol = "nfs";
    nfs.host = "nas.local";
    nfs.share = "/export";
    nfs.uid = 1000;
    nfs.gid = 1001;
    REQUIRE(locations.update(nfs).ok());

    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);

    splat::LocationEditScreen screen;
    splat::ScreenArgs args;
    args.a = "smb1";
    screen.on_enter(context, args);
    CHECK(screen.draft().protocol == "nfs");

    const splat::ListView& list = screen.list();
    const int uid = row_for_field(screen, splat::LocationEditScreen::Field::Uid);
    const int gid = row_for_field(screen, splat::LocationEditScreen::Field::Gid);
    REQUIRE(uid >= 0);
    REQUIRE(gid >= 0);
    // The value the location holds, not a dead expression that always reads "0".
    CHECK(list.rows()[static_cast<std::size_t>(uid)].right == "1000");
    CHECK(list.rows()[static_cast<std::size_t>(gid)].right == "1001");
    CHECK(list.rows()[static_cast<std::size_t>(uid)].label == "UID");
    CHECK(list.rows()[static_cast<std::size_t>(gid)].label == "GID");
}

TEST_CASE("location edit shows an NFS export's version and takes a typed uid") {
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);
    registry.add(splat::ScreenId::LocationEdit, [] {
        return std::unique_ptr<splat::Screen>(new splat::LocationEditScreen());
    });
    registry.add(splat::ScreenId::Keyboard, [] {
        return std::unique_ptr<splat::Screen>(new splat::KeyboardScreen());
    });
    REQUIRE(stack.push(splat::ScreenId::LocationEdit, splat::ScreenArgs{}));
    auto* edit = dynamic_cast<splat::LocationEditScreen*>(stack.top());
    REQUIRE(edit != nullptr);

    // A new location defaults to SMB; one Cross on Protocol makes it NFS.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Protocol));
    REQUIRE(edit->draft().protocol == "nfs");
    CHECK(edit->draft().uid == 65534);  // nobody, the default AUTH_SYS id
    CHECK(edit->draft().gid == 65534);
    CHECK(edit->draft().nfs_version == 3);

    // NFS hides the SMB credential rows and shows Export/Version instead.
    CHECK(row_for_field(*edit, splat::LocationEditScreen::Field::User) < 0);
    CHECK(row_for_field(*edit, splat::LocationEditScreen::Field::Password) < 0);
    CHECK(row_for_field(*edit, splat::LocationEditScreen::Field::Encrypt) < 0);
    const int share = row_for_field(*edit, splat::LocationEditScreen::Field::Share);
    REQUIRE(share >= 0);
    CHECK(edit->list().rows()[static_cast<std::size_t>(share)].label == "Export");

    // Version is a value row: Cross (and Left/Right) flips 3 <-> 4.
    const int version = row_for_field(*edit, splat::LocationEditScreen::Field::NfsVersion);
    REQUIRE(version >= 0);
    CHECK(edit->list().rows()[static_cast<std::size_t>(version)].right == "3");
    edit->activate(context, version);
    CHECK(edit->draft().nfs_version == 4);
    CHECK(edit->list().rows()[static_cast<std::size_t>(
              row_for_field(*edit, splat::LocationEditScreen::Field::NfsVersion))]
              .right == "4");

    // UID opens on the value it holds, so the field is cleared first; the
    // typed number replaces it.
    auto clear = [&context](splat::KeyboardScreen& keyboard) {
        for (int i = 0; i < 8; ++i) press_kind(keyboard, context, splat::KeyboardKey::Kind::Backspace);
    };
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Uid));
    auto* keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    clear(*keyboard);
    type_text(*keyboard, context, "1000");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().uid == 1000);
    CHECK(stack.top() == edit);

    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Gid));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    clear(*keyboard);
    type_text(*keyboard, context, "20");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().gid == 20);
}

TEST_CASE("location edit keeps a typed uid with no digits, and takes a fixed port") {
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    std::vector<std::string> toasts;
    context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(std::move(text)); };
    stack.set_context(&context);
    registry.add(splat::ScreenId::LocationEdit, [] {
        return std::unique_ptr<splat::Screen>(new splat::LocationEditScreen());
    });
    registry.add(splat::ScreenId::Keyboard, [] {
        return std::unique_ptr<splat::Screen>(new splat::KeyboardScreen());
    });
    REQUIRE(stack.push(splat::ScreenId::LocationEdit, splat::ScreenArgs{}));
    auto* edit = dynamic_cast<splat::LocationEditScreen*>(stack.top());
    REQUIRE(edit != nullptr);
    // The value the row holds is cleared first, then the typed text replaces it.
    auto clear = [&context](splat::KeyboardScreen& keyboard) {
        for (int i = 0; i < 8; ++i) press_kind(keyboard, context, splat::KeyboardKey::Kind::Backspace);
    };

    // A fixed port is a row: that is what a server without rpcbind needs
    // (the local nfsd/smbserver checks, and a NAS with a pinned port).
    const int port = row_for_field(*edit, splat::LocationEditScreen::Field::Port);
    REQUIRE(port >= 0);
    CHECK(edit->list().rows()[static_cast<std::size_t>(port)].right == "Default");
    edit->activate(context, port);
    auto* keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    clear(*keyboard);
    type_text(*keyboard, context, "1445");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().port == 1445);
    CHECK(stack.top() == edit);

    // NFS: a uid typed with no digit at all must keep the current value. It
    // must never fall through to 0, which is AUTH_SYS root on the export.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Protocol));
    REQUIRE(edit->draft().protocol == "nfs");
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Uid));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    clear(*keyboard);
    type_text(*keyboard, context, "abc");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().uid == 65534u);

    // A ten-digit id above 2^31 is typeable and lands in the draft whole.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Uid));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    clear(*keyboard);
    type_text(*keyboard, context, "4294967294");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().uid == 4294967294u);
    CHECK(edit->list().rows()[static_cast<std::size_t>(
              row_for_field(*edit, splat::LocationEditScreen::Field::Uid))]
              .right == "4294967294");

    // And Save hands both values to the store unchanged.
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Host));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    type_text(*keyboard, context, "nas.local");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Share));
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    type_text(*keyboard, context, "export");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    toasts.clear();
    edit->activate(context, row_for_field(*edit, splat::LocationEditScreen::Field::Save));
    REQUIRE(toasts.size() == 1);
    const std::vector<splat::LocationView> listed = locations.list();
    REQUIRE(listed.size() == 3);
    const splat::LocationView& added = listed.back();
    CHECK(added.protocol == "nfs");
    CHECK(added.port == 1445);
    CHECK(added.uid == 4294967294u);
}

TEST_CASE("the mount reports how long a dead share took (offline after N s)") {
    // T5: the console case asserts A3.2's "within 7 s" from this number, so it
    // has to come from the mount itself and not from the whole run's bound.
    splat::vfs::LocationStore store;
    splat::Result<splat::vfs::LocationStore> opened =
        splat::vfs::LocationStore::open(splat_test::TempDir("vfs-locations-time").path() / "l.json");
    REQUIRE(opened.ok());
    store = opened.take();

    splat::vfs::Location dead;
    dead.protocol = splat::vfs::LocationProtocol::nfs;
    dead.host = "127.0.0.1";
    dead.port = 1;
    dead.share = "/export";
    REQUIRE(store.add(dead).ok());

    auto& registry = splat::vfs::file_systems();
    struct Cleanup {
        ~Cleanup() { splat::vfs::file_systems().remove("nfs0"); }
    } cleanup;
    splat::LocationService service(store, registry, [] {});
    // The constructor started the mount worker; wait for its answer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (service.busy() && std::chrono::steady_clock::now() < deadline) {
        service.tick(1.0 / 60.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.tick(1.0 / 60.0);
    const std::string state = service.status("nfs0");
    MESSAGE("offline state: " << state);
    CHECK(state.compare(0, 8, "offline ") == 0);
    CHECK(state.find("offline after ") == 0);
    CHECK(state.find(" s:") != std::string::npos);
    // The number is the mount's own elapsed time, and it is small for a closed
    // local port: the whole point of the assertion.
    const std::size_t begin = std::string("offline after ").size();
    const double seconds = std::atof(state.c_str() + begin);
    CHECK(seconds < 7.0);
}

TEST_CASE("a location whose adapter broke stops saying ready, and recovers") {
    // BUG 1's second half: the adapter drops its connection after one bad SMB
    // reply, and the Locations screen used to keep saying ready. check_health()
    // reads IFileSystem::connection_error() on every tick.
    splat_test::TempDir dir("ui-location-health");
    splat::Result<splat::vfs::LocationStore> opened =
        splat::vfs::LocationStore::open(dir.path() / "locations.json");
    REQUIRE(opened.ok());
    splat::vfs::LocationStore store = opened.take();
    splat::vfs::Location nfs;
    nfs.protocol = splat::vfs::LocationProtocol::nfs;
    nfs.host = "127.0.0.1";
    nfs.port = 1;
    nfs.share = "/export";
    REQUIRE(store.add(nfs).ok());

    // The adapter is already registered, so the mount proves it through this
    // one instead of opening a socket.
    auto mounted = std::make_shared<FlatFakeFileSystem>("nfs0");
    splat::vfs::file_systems().add(mounted);
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("nfs0"); }
    } removed;

    splat::LocationService service(std::move(store), splat::vfs::file_systems());
    for (int i = 0; i < 500 && service.busy(); ++i) {
        service.tick(0.05);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE_MESSAGE(service.status("nfs0") == "ready", service.status("nfs0"));

    mounted->health_ = "nfs0: cannot list : connection reset";
    service.tick(0.05);
    MESSAGE("after the adapter broke: " << service.status("nfs0"));
    CHECK(service.status("nfs0").find("offline: ") == 0);
    CHECK(service.status("nfs0").find("connection reset") != std::string::npos);
    // The locations list (what the screen draws) carries the same state.
    const std::vector<splat::LocationView> listed = service.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed.front().state == service.status("nfs0"));

    // The adapter reconnected (its next use worked): the row goes back.
    mounted->health_.clear();
    service.tick(0.05);
    CHECK(service.status("nfs0") == "ready");
}

TEST_CASE("the Port field can be cleared back to the protocol default") {
    // L2: empty means "the protocol default". The keyboard's free-text rule
    // used to disable OK on an empty field, so a port typed once (1445) could
    // never be cleared again - the row's own hint said "0 = default" but only
    // typing a literal 0 got there.
    splat::FakeLocationService locations;
    splat::UiServices services;
    services.locations = &locations;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    context.content = splat::Rect{16, 78, 1248, 586};
    stack.set_context(&context);
    registry.add(splat::ScreenId::LocationEdit, [] {
        return std::unique_ptr<splat::Screen>(new splat::LocationEditScreen());
    });
    registry.add(splat::ScreenId::Keyboard, [] {
        return std::unique_ptr<splat::Screen>(new splat::KeyboardScreen());
    });
    REQUIRE(stack.push(splat::ScreenId::LocationEdit, splat::ScreenArgs{}));
    auto* edit = dynamic_cast<splat::LocationEditScreen*>(stack.top());
    REQUIRE(edit != nullptr);
    const int port = row_for_field(*edit, splat::LocationEditScreen::Field::Port);
    REQUIRE(port >= 0);

    // Type a fixed port first.
    edit->activate(context, port);
    auto* keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    type_text(*keyboard, context, "1445");
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    REQUIRE(edit->draft().port == 1445);
    REQUIRE(stack.top() == edit);
    CHECK(edit->list().rows()[static_cast<std::size_t>(port)].right == "1445");

    // Clear it: OK is enabled on an empty field, and the draft is back to the
    // default (0) with the row saying so.
    edit->activate(context, port);
    keyboard = dynamic_cast<splat::KeyboardScreen*>(stack.top());
    REQUIRE(keyboard != nullptr);
    for (int i = 0; i < 6; ++i) press_kind(*keyboard, context, splat::KeyboardKey::Kind::Backspace);
    CHECK(keyboard->drawn_text().empty());
    press_kind(*keyboard, context, splat::KeyboardKey::Kind::Ok);
    CHECK(edit->draft().port == 0);
    REQUIRE(stack.top() == edit);
    CHECK(edit->list().rows()[static_cast<std::size_t>(port)].right == "Default");
}
