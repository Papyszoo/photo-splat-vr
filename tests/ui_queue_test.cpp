// QueueScreen: the rows it builds from IJobService::snapshot(), the ViewScene
// command a done row emits (after looking the scene up with the album listing),
// Square on a queued/running job, the empty state and the 4 Hz repaint while a
// job runs.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/path.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/screens/queue.h"
#include "ui/screen_stack.h"

namespace {

using splat::JobView;

// A service whose views and cancel result the test controls, so every state
// (including failed, which FakeJobService cannot reach) can be put on screen.
class StubJobService : public splat::IJobService {
public:
    std::vector<JobView> views;
    std::vector<std::string> cancelled;

    splat::Status enqueue(const std::vector<splat::Path>&, const std::string&,
                          const std::string&) override {
        return splat::Status::success();
    }
    std::vector<JobView> snapshot() const override { return views; }
    splat::Status cancel(const std::string& id) override {
        cancelled.push_back(id);
        return splat::Status::success();
    }
    bool busy() const override {
        for (const JobView& view : views) {
            if (view.state == "running") return true;
        }
        return false;
    }
};

JobView make_view(const std::string& id, const std::string& name, const std::string& state,
                  const std::string& album = std::string()) {
    JobView view;
    view.id = id;
    view.name = name;
    view.backend = "sharp";
    view.album = album;
    view.state = state;
    view.stage = state;
    return view;
}

struct Fixture {
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    splat::QueueScreen screen;
    std::vector<splat::UiCommand> commands;
    std::vector<std::string> toasts;
    int invalidations = 0;

    Fixture() {
        context.content = splat::Rect{16, 78, 1248, 500};
        context.command_sink = [this](splat::UiCommand command) {
            commands.push_back(std::move(command));
        };
        context.toast_sink = [this](std::string text, double) { toasts.push_back(std::move(text)); };
        context.invalidate_sink = [this] { ++invalidations; };
    }

    void enter() { screen.on_enter(context, splat::ScreenArgs{}); }

    void press(uint32_t action, double dt = 0.0) {
        splat::UiInput input;
        input.pressed = action;
        input.dt = dt;
        screen.update(context, input);
    }

    void tick(double dt) {
        splat::UiInput input;
        input.dt = dt;
        screen.update(context, input);
    }
};

}  // namespace

TEST_CASE("ui_queue lists name, backend, album and stage for every job") {
    Fixture fixture;
    StubJobService jobs;

    JobView done = make_view("job-000001", "kitchen_12_aug.jpg", "done", "2026-10-26");
    JobView running = make_view("job-000002", "pizza.jpg", "running");
    running.stage = "inferring";
    running.fraction = 0.5f;
    JobView queued = make_view("job-000003", "garden.jpg", "queued");
    JobView failed = make_view("job-000004", "hallway.jpg", "failed", "2026-10-25");
    failed.message = "no reconstruction backend registered";
    jobs.views = {done, running, queued, failed};

    fixture.services.jobs = &jobs;
    fixture.enter();

    const std::vector<splat::ListRow>& rows = fixture.screen.list().rows();
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].label == "kitchen_12_aug.jpg");
    // Plain words: the model's name, the folder, and the state.
    CHECK(rows[0].sublabel == "SHARP  \xC2\xB7  2026-10-26");
    CHECK(rows[0].right == "Done");
    CHECK(rows[1].label == "pizza.jpg");
    CHECK(rows[1].sublabel == "SHARP  \xC2\xB7  Unsorted");
    CHECK(rows[1].right == "Converting 50%");
    CHECK(rows[2].right == "Queued");
    // A failure says why where the folder would be.
    CHECK(rows[3].sublabel == "no reconstruction backend registered");
    CHECK(rows[3].right == "Failed");

    CHECK(fixture.screen.title() == "Conversions");
    // The prompts follow the focused row: a done job can be viewed, a queued
    // or running one cancelled.
    std::vector<splat::Hint> hints = fixture.screen.hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Cross");
    CHECK(hints[0].label == "View");
    CHECK(hints[1].button == "Circle");
    CHECK(hints[1].label == "Back");
    fixture.press(splat::Down);
    hints = fixture.screen.hints();
    REQUIRE(hints.size() == 2);
    CHECK(hints[0].button == "Square");
    CHECK(hints[0].label == "Cancel");
    fixture.press(splat::Down);
    fixture.press(splat::Down);
    hints = fixture.screen.hints();
    REQUIRE(hints.size() == 1);
    CHECK(hints[0].label == "Back");
}

TEST_CASE("ui_queue Select on a done row emits ViewScene for its scene") {
    Fixture fixture;
    StubJobService jobs;

    // The runner names the scene after the photo stem; the second conversion of
    // the same name got "-2", and that is the one the job wrote.
    splat_test::TempDir dir("ui-queue-scenes");
    const splat::Path root = dir.path();
    std::error_code error;
    std::filesystem::create_directories(root / "2026-10-26" / "photo", error);
    std::filesystem::create_directories(root / "2026-10-26" / "photo-2", error);
    std::filesystem::create_directories(root / "kitchen_1", error);
    for (const splat::Path& folder : {root / "2026-10-26" / "photo", root / "2026-10-26" / "photo-2",
                                      root / "kitchen_1"}) {
        std::ofstream(folder / "scene.psplat").put('x');
    }
    fixture.services.paths.scenes = root;

    jobs.views = {make_view("job-000001", "photo.jpg", "done", "2026-10-26"),
                  make_view("job-000002", "kitchen 1.jpg", "done")};
    fixture.services.jobs = &jobs;
    fixture.enter();
    REQUIRE(fixture.screen.list().rows().size() == 2);

    fixture.press(splat::Select);
    REQUIRE(fixture.commands.size() == 1);
    CHECK(fixture.commands[0].kind == splat::UiCommand::ViewScene);
    CHECK(fixture.commands[0].album == "2026-10-26");
    // The scenes sort by folder name: "photo" then "photo-2".
    CHECK(fixture.commands[0].index == 1);

    // The loose-scene album ("" folder) resolves to the scene at the root.
    fixture.press(splat::Down);
    fixture.press(splat::Select);
    REQUIRE(fixture.commands.size() == 2);
    CHECK(fixture.commands[1].kind == splat::UiCommand::ViewScene);
    CHECK(fixture.commands[1].album.empty());
    CHECK(fixture.commands[1].index == 0);
    CHECK(fixture.toasts.empty());

    // A done row whose scene is gone says so instead of emitting a command.
    jobs.views = {make_view("job-000003", "missing.jpg", "done")};
    fixture.tick(0.1);
    fixture.press(splat::Select);
    CHECK(fixture.commands.size() == 2);
    REQUIRE(fixture.toasts.size() == 1);
    CHECK(fixture.toasts[0] == "scene is not there yet");
}

TEST_CASE("ui_queue Square cancels the focused queued or running job") {
    Fixture fixture;
    splat::FakeJobService jobs;
    REQUIRE(jobs.enqueue({splat::Path("a.jpg"), splat::Path("b.jpg")}, "sharp", "").ok());
    jobs.tick(1.0);  // a is running, b is queued

    fixture.services.jobs = &jobs;
    fixture.enter();
    REQUIRE(fixture.screen.list().rows().size() == 2);
    CHECK(fixture.screen.list().rows()[0].right == "Converting 25%");

    // The queued one goes away at once.
    fixture.press(splat::Down);
    fixture.press(splat::Secondary);
    const std::vector<JobView> after_queued = jobs.snapshot();
    REQUIRE(after_queued.size() == 2);
    CHECK(after_queued[1].state == "cancelled");
    CHECK(fixture.screen.list().rows()[1].right == "Cancelled");
    REQUIRE(fixture.toasts.size() == 1);
    CHECK(fixture.toasts[0] == "Cancelled");

    // The running one is a request: the row says it stops after this photo.
    fixture.press(splat::Up);
    fixture.press(splat::Secondary);
    CHECK(jobs.snapshot()[0].state == "cancelled");
    CHECK(fixture.screen.list().rows()[0].right == "Cancelled");
    REQUIRE(fixture.toasts.size() == 2);

    // A done row has nothing to cancel and is not silently ignored.
    StubJobService done_only;
    done_only.views = {make_view("job-000009", "done.jpg", "done", "")};
    fixture.services.jobs = &done_only;
    fixture.tick(0.1);
    fixture.press(splat::Secondary);
    CHECK(done_only.cancelled.empty());
    REQUIRE(fixture.toasts.size() == 3);
    CHECK(fixture.toasts[2] == "already done");
}

TEST_CASE("ui_queue shows the empty state and repaints while a job runs") {
    Fixture fixture;
    StubJobService jobs;
    fixture.services.jobs = &jobs;
    fixture.enter();
    CHECK(fixture.screen.list().rows().empty());

    splat::UiCanvas canvas(1280, 720);
    fixture.screen.draw(fixture.context, canvas, fixture.context.content);
    std::size_t ink = 0;
    const std::vector<uint8_t>& pixels = canvas.pixels();
    for (std::size_t index = 3; index < pixels.size(); index += 4) {
        if (pixels[index] != 0) ++ink;
    }
    CHECK(ink > 0);  // "Queue is empty" is drawn

    // Four repaints a second while a job runs, none when it is idle.
    jobs.views = {make_view("job-000001", "photo.jpg", "running")};
    fixture.tick(0.1);
    fixture.tick(0.1);
    CHECK(fixture.invalidations == 0);
    fixture.tick(0.1);
    CHECK(fixture.invalidations == 1);
    fixture.tick(0.25);
    CHECK(fixture.invalidations == 2);

    jobs.views = {make_view("job-000001", "photo.jpg", "done")};
    fixture.tick(1.0);
    CHECK(fixture.invalidations == 2);

    // A missing service is reported instead of showing an empty queue.
    fixture.services.jobs = nullptr;
    fixture.tick(0.1);
    CHECK(fixture.screen.jobs().empty());
    CHECK(fixture.screen.list().rows().empty());
}
