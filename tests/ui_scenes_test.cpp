// ScenesScreen: listing order, the tile line (model, splat count, seconds), the
// album title, ViewScene with the album and index, Square -> AlbumPick args,
// the focus restore on return, and the labelled placeholder a scene without a
// thumbnail gets instead of an error.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <cmath>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/image.h"
#include "common/path.h"
#include "jobs/image_io.h"
#include "scene/scene_folder.h"
#include "scene/scene_view.h"
#include "test_util.h"
#include "tree_filesystem.h"
#include "ui/fakes.h"
#include "ui/screens/placeholder.h"
#include "ui/screens/scenes.h"
#include "ui/screen_stack.h"
#include "vfs/io.h"

using splat::Path;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// A real JPEG thumbnail of one colour, so a test can tell the old image from
// the new one after a turn rewrote the file on disk.
void write_solid_thumb(const Path& folder, uint8_t r, uint8_t g, uint8_t b) {
    splat::Image image;
    image.resize(16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
        }
    }
    const auto encoded = splat::encode_jpeg(image, 95);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    std::ofstream out(splat::scene_files(folder).thumbnail, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encoded.value().data()),
              static_cast<std::streamsize>(encoded.value().size()));
    REQUIRE(static_cast<bool>(out));
}

void write_scene(const Path& folder, const char* backend, size_t splats, double seconds,
                 bool thumb, bool imported = false) {
    std::filesystem::create_directories(folder);
    splat::SceneMeta meta;
    meta.backend = backend;
    meta.seconds_taken = seconds;
    if (imported) {
        meta.origin_format = "splat";
        meta.origin_count = static_cast<uint32_t>(splats);
    }
    const splat::SplatScene scene = splat_test::make_test_scene(splats);
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    const splat::Status status = splat::write_scene_folder(folder, scene, meta, jpeg, jpeg);
    REQUIRE_MESSAGE(status.ok(), status.message());
    if (!thumb) std::filesystem::remove(splat::scene_files(folder).thumbnail);
}

class CapturingScreen : public splat::Screen {
public:
    void on_enter(splat::UiContext&, const splat::ScreenArgs& args) override {
        last = args;
        ++enters;
    }
    void update(splat::UiContext&, const splat::UiInput&) override {}
    void draw(splat::UiContext&, splat::UiCanvas&, const splat::Rect&) override {}
    std::string title() const override { return "stub"; }

    splat::ScreenArgs last;
    int enters = 0;
};

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    return input;
}

struct Fixture {
    splat_test::TempDir temp{"ui-scenes"};
    Path root = temp.path() / "scenes";
    splat::UiServices services;
    splat::FakeJobService jobs;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<splat::UiCommand> commands;
    splat::ScenesScreen* scenes = nullptr;
    CapturingScreen* album_pick = nullptr;
    CapturingScreen* photos = nullptr;
    CapturingScreen* keyboard = nullptr;

    Fixture() {
        std::filesystem::create_directories(root);
        services.paths.scenes = root;
        services.jobs = &jobs;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.command_sink = [this](splat::UiCommand command) {
            commands.push_back(std::move(command));
        };
        stack.set_context(&context);

        registry.add(splat::ScreenId::Scenes, [this] {
            auto screen = std::make_unique<splat::ScenesScreen>();
            scenes = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::AlbumPick, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            album_pick = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Photos, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            photos = screen.get();
            return screen;
        });
        registry.add(splat::ScreenId::Keyboard, [this] {
            auto screen = std::make_unique<CapturingScreen>();
            keyboard = screen.get();
            return screen;
        });
    }

    void enter(const std::string& album) {
        splat::ScreenArgs args;
        args.a = album;
        REQUIRE(stack.push(splat::ScreenId::Scenes, args));
    }
};

}  // namespace

TEST_CASE("ui_scenes lists an album in folder order with model, splats and seconds") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "a-kitchen", "sharp", 1234, 12.5, false);
    write_scene(fixture.root / "trip" / "b-pizza", "depth-anything-v2-small", 213, 0.4, false);
    write_scene(fixture.root / "trip" / "c-garden-night", "sharp", 5, 3.0, true);
    write_scene(fixture.root / "loose", "sharp", 12, 1.0, false);
    fixture.enter("trip");

    CHECK(fixture.scenes->album() == "trip");
    CHECK(fixture.scenes->title() == "trip");

    const std::vector<std::string> names = fixture.scenes->names();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "a-kitchen");
    CHECK(names[1] == "b-pizza");
    CHECK(names[2] == "c-garden-night");

    // The "+" tile comes first, then the scenes; focus starts on the first scene.
    const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
    REQUIRE(cells.size() == 4);
    CHECK(cells[0].icon == splat::GridCell::Icon::Add);
    CHECK(cells[0].caption == "Add photos");
    CHECK(fixture.scenes->first_scene_cell() == 1);
    CHECK(fixture.scenes->grid().focus() == 1);
    CHECK(fixture.scenes->focused_scene() == 0);
    CHECK(cells[1].caption == "a-kitchen");
    // The scene's info is the model only (users compare models; counts and
    // timings stay out of the shelf), spelled as the Models screen does, and
    // drawn under the focused scene only.
    CHECK(cells[1].sublabel == "SHARP");
    CHECK(cells[2].sublabel == "DA2-Small");
    CHECK(cells[3].sublabel == "SHARP");
    CHECK(fixture.scenes->grid().sublabel_focus_only);
    // A long name gets two lines in a fixed label area (one label rule: the
    // same number of lines focused or not).
    CHECK(fixture.scenes->grid().caption_lines == splat::kThumbnailCaptionLines);
    CHECK(fixture.scenes->grid().caption_lines == 2);

    const std::vector<splat::Hint> hints = fixture.scenes->hints();
    // A scene entry turns with the same two keys as a photo entry (L3/R3); the
    // row keeps View, More and Back around them.
    REQUIRE(hints.size() == 5);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "View");
    CHECK(hints[1].button == "Triangle");
    CHECK(hints[1].label == "More");
    CHECK(hints[2].button == "L3");
    CHECK(hints[2].label == "Rotate left");
    CHECK(hints[3].button == "R3");
    CHECK(hints[3].label == "Rotate right");
    CHECK(hints[4].button == "Circle");
    CHECK(hints[4].label == "Back");
}

TEST_CASE("ui_scenes titles the loose scene album Unsorted") {
    Fixture fixture;
    write_scene(fixture.root / "loose", "sharp", 12, 1.0, false);
    fixture.enter("");

    CHECK(fixture.scenes->album().empty());
    CHECK(fixture.scenes->title() == "Unsorted");
    REQUIRE(fixture.scenes->names().size() == 1);
}

TEST_CASE("ui_scenes select emits ViewScene with the album and index") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 2.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.commands.size() == 1);
    CHECK(fixture.commands[0].kind == splat::UiCommand::ViewScene);
    CHECK(fixture.commands[0].album == "trip");
    CHECK(fixture.commands[0].index == 1);
    // Viewing never navigates the stack by itself: the host owns the scene.
    CHECK(fixture.stack.top_id() == splat::ScreenId::Scenes);

    // The loose scenes travel as the empty album key, with their own index.
    Fixture loose;
    write_scene(loose.root / "a", "sharp", 4, 1.0, false);
    write_scene(loose.root / "b", "sharp", 4, 1.0, false);
    loose.enter("");
    loose.stack.update(loose.context, press(splat::Select));
    REQUIRE(loose.commands.size() == 1);
    CHECK(loose.commands[0].album.empty());
    CHECK(loose.commands[0].index == 0);
}

TEST_CASE("ui_scenes triangle asks to move the focused scene") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Context));
    // The scene's menu ("More"): Move to folder, Rename, Delete.
    REQUIRE(fixture.scenes->menu().is_open());
    REQUIRE(fixture.scenes->menu().items().size() == 3);
    CHECK(fixture.scenes->menu().items()[0] == "Move to folder");
    CHECK(fixture.scenes->menu().items()[1] == "Rename");
    CHECK(fixture.scenes->menu().items()[2] == "Delete");
    CHECK(fixture.scenes->menu().title() == "s2");
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.album_pick != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::AlbumPick);
    CHECK(fixture.album_pick->enters == 1);
    CHECK(fixture.album_pick->last.a == "move-scene");
    CHECK(fixture.album_pick->last.b == (fixture.root / "trip" / "s2").string());
    // Triangle alone does not view the scene.
    CHECK(fixture.commands.empty());
}

TEST_CASE("ui_scenes renames and deletes a scene from its menu") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    // Rename opens the keyboard for that scene folder.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.keyboard != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Keyboard);
    CHECK(fixture.keyboard->last.a == "rename-scene");
    CHECK(fixture.keyboard->last.b == (fixture.root / "trip" / "s1").string());
    REQUIRE(fixture.stack.pop());

    // Delete asks first, with Cancel focused: Cross there keeps the scene.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.scenes->confirm().is_open());
    CHECK(fixture.scenes->confirm().title() == "Delete “s1”?");
    CHECK(fixture.scenes->confirm().focus() == 0);
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(fixture.scenes->confirm().is_open());
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s1"));

    // Circle closes the card too.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.scenes->handle_back(fixture.context));
    CHECK_FALSE(fixture.scenes->confirm().is_open());

    // Right, Cross: the scene's folder (and only it) is gone; the grid
    // re-lists.
    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Down));
    fixture.stack.update(fixture.context, press(splat::Select));
    fixture.stack.update(fixture.context, press(splat::Right));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "trip" / "s1"));
    CHECK(std::filesystem::is_directory(fixture.root / "trip" / "s2"));
    CHECK(std::filesystem::is_directory(fixture.root / "trip"));
    REQUIRE(fixture.scenes->names().size() == 1);
    CHECK(fixture.scenes->names()[0] == "s2");
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Deleted s1");
}

// An imported splat has no conversion step, so it keeps the saved rotation
// (DECISIONS.md): Square turns it a quarter, writes the scene's own meta.json
// and toasts the angle it landed on, so the VR viewer and the next TV run show
// the scene turned. Pressing it again cycles, and nothing else on the row
// (view, menu) fires.
TEST_CASE("ui_scenes offers the same L3/R3 turn on a converted and an imported scene") {
    // The turn is a file action for every entry, not an imported-scene feature
    // (DECISIONS.md, "Change 2026-11-05"): the same two keys and the same hint
    // on both, and Square is no longer an action on a scene row.
    Fixture fixture;
    write_scene(fixture.root / "trip" / "a-import", "import", 4, 1.0, true, true);
    write_scene(fixture.root / "trip" / "b-converted", "sharp", 4, 1.0, true);
    fixture.enter("trip");

    const std::vector<splat::Hint> imported = fixture.scenes->hints();
    bool left = false;
    bool right = false;
    for (const splat::Hint& hint : imported) {
        if (hint.button == "L3" && hint.label == "Rotate left") left = true;
        if (hint.button == "R3" && hint.label == "Rotate right") right = true;
        CHECK(hint.button != "Square");
    }
    CHECK(left);
    CHECK(right);

    fixture.stack.update(fixture.context, press(splat::Right));
    CHECK(fixture.scenes->names()[static_cast<std::size_t>(fixture.scenes->focused_scene())] ==
          "b-converted");
    const std::vector<splat::Hint> converted = fixture.scenes->hints();
    left = right = false;
    for (const splat::Hint& hint : converted) {
        if (hint.button == "L3" && hint.label == "Rotate left") left = true;
        if (hint.button == "R3" && hint.label == "Rotate right") right = true;
    }
    CHECK(left);
    CHECK(right);
}

TEST_CASE("ui_scenes queues a left or right scene turn as a background job") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    const Path folder = fixture.root / "trip" / "s1";
    write_scene(folder, "sharp", 4, 1.0, true);
    fixture.enter("trip");

    // L3 is a quarter turn left: three clockwise quarters, the queue's signed
    // range (job_queue.h normalises to 0..3).
    fixture.stack.update(fixture.context, press(splat::RotateLeft));
    CHECK(fixture.jobs.last_rotate_quarters() == 3);
    CHECK(fixture.jobs.last_rotate_scene() == folder);
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back().rfind("Rotating s1", 0) == 0);
    // The tile says the entry is being written, and the top bar is not told
    // this is a conversion.
    const splat::GridView& grid = fixture.scenes->grid();
    REQUIRE(grid.cells().size() >= 2);
    CHECK(grid.cells()[1].badge == "Rotating");

    // R3 is the other direction; the queue refuses a second turn of the same
    // scene while the first is active, so let it finish first.
    fixture.jobs.tick(splat::FakeJobService::kSecondsPerJob + 0.1);
    fixture.stack.update(fixture.context, press(0));
    fixture.stack.update(fixture.context, press(splat::RotateRight));
    CHECK(fixture.jobs.last_rotate_quarters() == 1);
}

TEST_CASE("ui_scenes refuses a turn while the scene is open or already being turned") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    const Path folder = fixture.root / "trip" / "s1";
    write_scene(folder, "sharp", 4, 1.0, true);
    fixture.enter("trip");

    // Open in the viewer: the renderer holds the version it read, so a rewrite
    // under it would leave the picture and the file disagreeing.
    fixture.services.open_scene = folder;
    fixture.stack.update(fixture.context, press(splat::RotateRight));
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Close the scene before rotating it");
    CHECK(fixture.jobs.last_rotate_scene().empty());

    // Busy: the same scene already has a turn queued or running.
    fixture.services.open_scene.clear();
    fixture.stack.update(fixture.context, press(splat::RotateRight));
    REQUIRE(fixture.jobs.last_rotate_scene() == folder);
    fixture.stack.update(fixture.context, press(splat::RotateRight));
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "This scene is already being rotated");
}

TEST_CASE("ui_scenes reports a finished turn and drops the stale thumbnail") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, true);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::RotateRight));
    fixture.jobs.tick(splat::FakeJobService::kSecondsPerJob + 0.1);
    fixture.stack.update(fixture.context, press(0));
    bool said = false;
    for (const std::string& toast : toasts) said = said || toast == "Rotated s1";
    CHECK(said);
    // The entry is no longer marked busy.
    const splat::GridView& grid = fixture.scenes->grid();
    REQUIRE(grid.cells().size() >= 2);
    CHECK(grid.cells()[1].badge.empty());
}

TEST_CASE("ui_scenes refuses to open a scene whose turn is still queued") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    const Path folder = fixture.root / "trip" / "s1";
    write_scene(folder, "sharp", 4, 1.0, true);
    fixture.enter("trip");

    // The turn is queued behind another job; Cross would load the folder the
    // runner is about to rewrite, which is the state the task forbids. The
    // runner has its own guard for the queue-behind-a-conversion case; this is
    // the shelf's half of it.
    fixture.stack.update(fixture.context, press(splat::RotateRight));
    REQUIRE(fixture.jobs.last_rotate_scene() == folder);
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.commands.empty());
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "This scene is being rotated");
}

TEST_CASE("ui_scenes reloads a thumbnail when a turn finished under another screen") {
    // A turn that ends while the shelf is covered leaves the cached pre-turn
    // image: on_enter()/apply_scenes() reset the busy mark without an observer
    // seeing the transition. The returned shelf must read the new file, not the
    // cache.
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    const Path folder = fixture.root / "trip" / "s1";
    write_scene(folder, "sharp", 4, 1.0, true);
    write_solid_thumb(folder, 250, 20, 20);
    fixture.enter("trip");

    const auto settle = [&fixture]() -> const splat::UiImage* {
        for (int attempt = 0; attempt < 400; ++attempt) {
            fixture.stack.update(fixture.context, press(0));
            const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
            if (cells.size() >= 2 && cells[1].thumb != nullptr && !cells[1].thumb->empty()) {
                return cells[1].thumb;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return nullptr;
    };

    const splat::UiImage* before = settle();
    REQUIRE(before != nullptr);
    const uint8_t before_red = before->pixel(0, 0)[0];

    fixture.stack.update(fixture.context, press(splat::RotateRight));
    // Cover the shelf; the job finishes while it is not the top screen.
    splat::ScreenArgs args;
    REQUIRE(fixture.stack.push(splat::ScreenId::Keyboard, args));
    fixture.jobs.tick(splat::FakeJobService::kSecondsPerJob + 0.1);
    // The real runner rewrote the file; the fake does not touch it, so the test
    // makes the visible change itself.
    write_solid_thumb(folder, 20, 20, 250);
    REQUIRE(fixture.stack.pop());

    const splat::UiImage* after = settle();
    REQUIRE(after != nullptr);
    CHECK(after->pixel(0, 0)[2] > 200);   // the new blue file
    CHECK(after->pixel(0, 0)[0] < 60);
    CHECK(before_red > 200);              // the old file really was red
    // The completion is announced on the way back.
    bool said = false;
    for (const std::string& toast : toasts) said = said || toast == "Rotated s1";
    CHECK(said);
}

TEST_CASE("ui_scenes keeps the focused scene after returning") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s2", "sharp", 4, 1.0, false);
    write_scene(fixture.root / "trip" / "s3", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Right));
    REQUIRE(fixture.scenes->focused_scene() == 1);
    CHECK(fixture.scenes->names()[1] == "s2");

    fixture.stack.update(fixture.context, press(splat::Context));
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.stack.top_id() == splat::ScreenId::AlbumPick);
    REQUIRE(fixture.stack.pop());

    // The pop revealed the screen again: it re-listed the disk and put the focus
    // back on the scene the user was on.
    CHECK(fixture.scenes->focused_scene() == 1);
    CHECK(fixture.scenes->names()[1] == "s2");
}

TEST_CASE("ui_scenes plus opens the photo picker for this folder") {
    Fixture fixture;
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    fixture.enter("trip");

    fixture.stack.update(fixture.context, press(splat::Left));  // the "+" tile
    CHECK(fixture.scenes->focused_scene() == -1);
    CHECK(fixture.scenes->hints()[0].label == "Add photos");
    fixture.stack.update(fixture.context, press(splat::Select));
    REQUIRE(fixture.photos != nullptr);
    CHECK(fixture.stack.top_id() == splat::ScreenId::Photos);
    CHECK(fixture.photos->last.a == "trip");
    CHECK(fixture.commands.empty());

    // An empty folder starts on the "+" tile.
    Fixture empty;
    std::filesystem::create_directories(empty.root / "new");
    empty.enter("new");
    CHECK(empty.scenes->grid().cells().size() == 1);
    CHECK(empty.scenes->grid().focus() == 0);
}

TEST_CASE("ui_scenes shows conversions into this folder until they finish") {
    Fixture fixture;
    splat::FakeJobService jobs;
    fixture.services.jobs = &jobs;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);
    REQUIRE(jobs.enqueue({Path("one.jpg"), Path("two.jpg")}, "sharp", "trip").ok());
    REQUIRE(jobs.enqueue({Path("elsewhere.jpg")}, "sharp", "other").ok());
    jobs.tick(1.0);  // one.jpg at 25 %
    fixture.enter("trip");

    // "+", the two pending photos, then the scene; the other folder's job is
    // not here.
    REQUIRE(fixture.scenes->pending() == 2);
    const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
    REQUIRE(cells.size() == 4);
    CHECK(cells[1].caption == "one");
    CHECK(cells[1].badge == "Converting");
    CHECK(cells[1].progress == doctest::Approx(0.25f));
    CHECK(cells[2].caption == "two");
    CHECK(cells[2].badge == "Queued");
    CHECK(cells[2].progress == 0.0f);
    CHECK(fixture.scenes->focused_scene() == 0);
    CHECK(fixture.scenes->grid().focus() == 3);

    // Progress moves in place, without a re-list.
    jobs.tick(1.0);
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->grid().cells()[1].progress == doctest::Approx(0.5f));
    CHECK(fixture.scenes->focused_scene() == 0);

    // Cross on a pending tile says what it is doing, it does not view.
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.commands.empty());
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back().find("one.jpg") == 0);

    // one.jpg finishes (the fake writes no scene): the folder re-lists and the
    // tile is gone; a failed job says so once.
    jobs.tick(3.0);
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->pending() == 1);
    REQUIRE(jobs.cancel(jobs.snapshot()[1].id).ok());
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->pending() == 0);
    CHECK(fixture.scenes->grid().cells().size() == 2);
}

TEST_CASE("ui_scenes counts a scene waiting for its network folder as a pending tile") {
    Fixture fixture;
    std::vector<std::string> toasts;
    fixture.context.toast_sink = [&toasts](std::string text, double) { toasts.push_back(text); };
    write_scene(fixture.root / "trip" / "s1", "sharp", 4, 1.0, false);

    splat::JobView waiting;
    waiting.name = "one.jpg";
    waiting.photo = Path("one.jpg");
    waiting.album = "trip";
    waiting.kind = "convert";
    waiting.state = "waiting";
    waiting.message = "Waiting for NAS";
    waiting.output_root = Path("smb0:/out");  // saved on the Output folder
    fixture.jobs.inject_job(waiting);
    waiting.name = "elsewhere.jpg";
    waiting.album = "other";
    fixture.jobs.inject_job(waiting);
    fixture.enter("trip");

    // "+", the waiting photo, then the scene; the other folder's job is not here.
    REQUIRE(fixture.scenes->pending() == 1);
    const std::vector<splat::GridCell>& cells = fixture.scenes->grid().cells();
    REQUIRE(cells.size() == 3);
    CHECK(cells[1].caption == "one");
    CHECK(cells[1].badge == "Waiting");
    CHECK(cells[1].progress == 0.0f);

    // Cross on it says which share it waits for; it does not view anything.
    fixture.stack.update(fixture.context, press(splat::Left));
    fixture.stack.update(fixture.context, press(splat::Select));
    CHECK(fixture.commands.empty());
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts.back() == "Waiting for NAS");

    // The share is back and the scene is saved: the tile goes.
    fixture.jobs.set_job_state(fixture.jobs.snapshot()[0].id, "done");
    fixture.stack.update(fixture.context, splat::UiInput{});
    CHECK(fixture.scenes->pending() == 0);
}

TEST_CASE("ui_scenes a network shelf shows only the jobs saved into its own library") {
    struct Remove {
        ~Remove() { splat::vfs::file_systems().remove("smb0"); }
    } removed;
    auto share = std::make_shared<splat_test::TreeFileSystem>("smb0");
    share->files["lib/Trips/s1/scene.psplat"] = {1};
    splat::vfs::file_systems().add(share);

    Fixture fixture;
    splat::JobView job;
    job.photo = Path("one.jpg");
    job.name = "one.jpg";
    job.album = "Trips";
    job.kind = "convert";
    job.state = "waiting";
    job.message = "Waiting for NAS";
    job.output_root = Path("smb0:/lib");  // this library
    fixture.jobs.inject_job(job);
    job.name = "two.jpg";
    job.output_root = Path("smb0:/other");  // another folder of the share
    fixture.jobs.inject_job(job);
    job.name = "three.jpg";
    job.output_root = Path();  // the console's own album of the same name
    job.state = "queued";
    fixture.jobs.inject_job(job);

    splat::ScreenArgs args;
    args.a = "Trips";
    args.b = "smb0:/lib";
    REQUIRE(fixture.stack.push(splat::ScreenId::Scenes, args));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (fixture.scenes->names().empty() &&
           std::chrono::steady_clock::now() < deadline) {
        fixture.stack.update(fixture.context, splat::UiInput{});
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(fixture.scenes->names().size() == 1);
    REQUIRE(fixture.scenes->pending() == 1);
    CHECK(fixture.scenes->grid().cells()[1].caption == "one");
    CHECK(fixture.scenes->grid().cells()[1].badge == "Waiting");
}

TEST_CASE("ui_scenes draws one calm placeholder for a scene without a thumbnail") {
    // Every missing thumbnail is the same picture placeholder, whatever the
    // name: no initials, no colour per scene.
    splat::UiCanvas first(192, 112);
    splat::UiCanvas second(192, 112);
    splat::draw_placeholder(first, splat::Rect{0, 0, 192, 112}, 12);
    splat::draw_placeholder(second, splat::Rect{0, 0, 192, 112}, 12);
    CHECK(first.pixels() == second.pixels());
    // The rounded corner is cut; the fill is opaque; the picture glyph is
    // lighter than the fill.
    CHECK(first.pixels()[3] == 0);
    const uint8_t* fill = first.pixels().data() + (10 * 192 + 10) * 4;
    CHECK(fill[3] == 255);
    int lighter = 0;
    for (std::size_t index = 0; index < first.pixels().size(); index += 4) {
        if (first.pixels()[index + 3] == 255 && first.pixels()[index] > fill[0] + 30) ++lighter;
    }
    CHECK(lighter > 50);

    // A scene without thumb.jpg gets no thumbnail in its cell: the grid draws
    // this placeholder for it.
    Fixture fixture;
    write_scene(fixture.root / "trip" / "plain", "sharp", 4, 1.0, false);
    fixture.enter("trip");
    REQUIRE(fixture.scenes->grid().cells().size() == 2);
    const splat::GridCell& cell = fixture.scenes->grid().cells()[1];
    CHECK(cell.icon == splat::GridCell::Icon::None);
    CHECK((cell.thumb == nullptr || cell.thumb->empty()));
}
