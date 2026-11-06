// The job queue under a visible-folders policy: a row is shown or hidden by the
// album/location it writes to (stage U1: a job's photo source decides nothing),
// the refusal an enqueue gets, and the failed job that must not toast a hidden
// name.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/path.h"
#include "gallery/visibility.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "test_util.h"
#include "ui/fakes.h"
#include "ui/job_service.h"
#include "ui/screens/queue.h"
#include "ui/screens/scenes.h"
#include "ui/screen_stack.h"
#include "ui/widgets.h"

namespace {

using splat::JobView;
using splat::Path;
using splat::Status;
using splat::Visibility;
using splat::VisibilityContext;
using splat::VisibilityKey;

splat::UiInput press(uint32_t action) {
    splat::UiInput input;
    input.pressed = action;
    input.dt = 1.0 / 60.0;
    return input;
}

struct Fixture {
    splat_test::TempDir dir{"ui-queue-vis"};
    Path base = dir.path();
    splat::FakeJobService fake;
    splat::UiServices services;
    splat::UiTheme theme = splat::default_theme();
    splat::ScreenRegistry registry;
    splat::ScreenStack stack{registry};
    splat::UiContext context{services, theme, stack};
    std::vector<std::string> toasts;
    std::unique_ptr<splat::VisibilityScope> scope;

    Fixture() {
        services.paths.scenes = base / "scenes";
        services.paths.inbox = base / "data" / "ps5-splat-vr" / "inbox";
        services.paths.state = base / "data" / "ps5-splat-vr" / "state";
        services.paths.mnt = base / "mnt";
        services.paths.data = base / "data";
        services.jobs = &fake;
        context.content = splat::Rect{16, 78, 1248, 586};
        context.toast_sink = [this](std::string text, double) {
            toasts.push_back(std::move(text));
        };
        stack.set_context(&context);
        std::error_code error;
        std::filesystem::create_directories(services.paths.scenes / "Family A", error);
        install(Visibility::all_visible(context_of()));
    }

    VisibilityContext context_of() const {
        VisibilityContext out;
        out.scenes_root = services.paths.scenes;
        out.inbox = services.paths.inbox;
        out.mnt = services.paths.mnt;
        out.data = services.paths.data;
        out.app_data = services.paths.inbox.parent_path();
        return out;
    }

    void install(Visibility state) {
        scope.reset();
        scope = std::make_unique<splat::VisibilityScope>(std::move(state));
    }

    void enable(std::initializer_list<VisibilityKey> keys) {
        Visibility state = Visibility::all_visible(context_of());
        state.enabled = true;
        for (const VisibilityKey& key : keys) state.allowed.push_back(key);
        std::sort(state.allowed.begin(), state.allowed.end());
        install(std::move(state));
    }

    // One row per kind of hidden job: a hidden album and a hidden remote
    // destination. The same rows from a photo folder that is not ticked (there
    // is no such tick any more) are listed.
    void inject_three() {
        JobView visible;
        visible.id = "j-visible";
        visible.name = "visible.jpg";
        visible.photo = base / "data" / "Photos" / "visible.jpg";
        visible.backend = "da2";
        visible.album = "Family A";
        visible.kind = "convert";
        visible.state = "queued";
        visible.source_folder = "folder:" + (base / "data" / "Photos").generic_string();
        fake.inject_job(visible);

        JobView hidden_source;
        hidden_source.id = "j-source";
        hidden_source.name = "hidden-source.jpg";
        hidden_source.photo = base / "data" / "Hidden" / "hidden-source.jpg";
        hidden_source.backend = "da2";
        hidden_source.album = "Family A";
        hidden_source.kind = "convert";
        hidden_source.state = "failed";
        hidden_source.message = "boom";
        hidden_source.source_folder = "folder:" + (base / "data" / "Hidden").generic_string();
        fake.inject_job(hidden_source);

        JobView hidden_album;
        hidden_album.id = "j-album";
        hidden_album.name = "hidden-album.jpg";
        hidden_album.photo = base / "data" / "Photos" / "hidden-album.jpg";
        hidden_album.backend = "da2";
        hidden_album.album = "Family B";
        hidden_album.kind = "convert";
        hidden_album.state = "done";
        hidden_album.source_folder = "folder:" + (base / "data" / "Photos").generic_string();
        fake.inject_job(hidden_album);

        JobView hidden_share;
        hidden_share.id = "j-share";
        hidden_share.name = "hidden-share.jpg";
        hidden_share.photo = base / "data" / "Photos" / "hidden-share.jpg";
        hidden_share.backend = "da2";
        hidden_share.album = "Family A";
        hidden_share.kind = "convert";
        hidden_share.state = "queued";
        hidden_share.source_folder = "folder:" + (base / "data" / "Photos").generic_string();
        hidden_share.output_root = Path("smb1:/scenes");
        fake.inject_job(hidden_share);
    }

    bool has_toast(const std::string& text) const {
        for (const std::string& entry : toasts) {
            if (entry == text) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("ui_queue_visibility the queue screen shows no hidden destination row") {
    Fixture fixture;
    fixture.enable({VisibilityKey::album("Family A"),
                    VisibilityKey::console_folder(fixture.base / "data" / "Photos"),
                    VisibilityKey::location("smb0")});
    fixture.inject_three();
    splat::QueueScreen queue;
    queue.on_enter(fixture.context, splat::ScreenArgs{});
    // visible.jpg and hidden-source.jpg are both written into the ticked album,
    // so both are rows: the photo folder they came from decides nothing.
    REQUIRE(queue.jobs().size() == 2);
    CHECK(queue.jobs()[0].name == "visible.jpg");
    CHECK(queue.jobs()[1].name == "hidden-source.jpg");
    // The unticked album and the unticked share are gone.
    for (const splat::ListRow& row : queue.list().rows()) {
        CHECK(row.label != "hidden-album.jpg");
        CHECK(row.label != "hidden-share.jpg");
        CHECK(row.sublabel.find("Family B") == std::string::npos);
    }

    // The top bar's chip counts the visible rows only: one queued job (the
    // second visible row failed already).
    const splat::ConversionStatus status = splat::conversion_status(&fixture.fake);
    CHECK(status.active == 1);
    // With nothing visible the chip is empty.
    fixture.enable({});
    const splat::ConversionStatus none = splat::conversion_status(&fixture.fake);
    CHECK(none.active == 0);
    CHECK(none.queued == 0);
}

TEST_CASE("ui_queue_visibility a hidden failed job never toasts") {
    Fixture fixture;
    fixture.enable({VisibilityKey::album("Family A")});
    JobView job;
    job.id = "j1";
    job.name = "hidden-album.jpg";
    job.photo = fixture.base / "data" / "Photos" / "hidden-album.jpg";
    job.backend = "da2";
    job.album = "Family B";  // the destination is the hidden one
    job.kind = "convert";
    job.state = "queued";
    job.source_folder = "folder:" + (fixture.base / "data" / "Photos").generic_string();
    fixture.fake.inject_job(job);

    splat::ScenesScreen scenes;
    splat::ScreenArgs args;
    args.a = "Family A";
    scenes.on_enter(fixture.context, args);
    CHECK(scenes.pending() == 0);  // the queued row is hidden from the start

    // It fails: still hidden, so no toast and no row.
    fixture.fake.set_job_state("j1", "failed", "boom");
    scenes.update(fixture.context, press(splat::Down));
    CHECK(fixture.toasts.empty());
    CHECK(scenes.pending() == 0);

}

TEST_CASE("ui_queue_visibility a job from a photo source is listed with its album ticked") {
    // Stage U1: the source half is gone. A photo from a console folder that was
    // never ticked (and can no longer be) is a normal row in a ticked album,
    // and it is not announced when it fails; with the album unticked the whole
    // row is hidden.
    Fixture fixture;
    fixture.enable({VisibilityKey::album("Family A")});
    JobView job;
    job.id = "j1";
    job.name = "hidden-source.jpg";
    job.photo = fixture.base / "data" / "Hidden" / "hidden-source.jpg";
    job.backend = "da2";
    job.album = "Family A";
    job.kind = "convert";
    job.state = "queued";
    job.source_folder = "folder:" + (fixture.base / "data" / "Hidden").generic_string();
    fixture.fake.inject_job(job);

    splat::QueueScreen queue;
    queue.on_enter(fixture.context, splat::ScreenArgs{});
    REQUIRE(queue.jobs().size() == 1);
    CHECK(queue.jobs()[0].name == "hidden-source.jpg");

    splat::ScenesScreen scenes;
    splat::ScreenArgs args;
    args.a = "Family A";
    scenes.on_enter(fixture.context, args);
    REQUIRE(scenes.pending() == 1);
    fixture.fake.set_job_state("j1", "failed", "boom");
    scenes.update(fixture.context, press(splat::Down));
    CHECK(fixture.has_toast("Could not convert hidden-source.jpg: boom"));
    CHECK(scenes.pending() == 0);

    // The album is the rule: without it, the row and its message are gone.
    fixture.enable({});
    splat::QueueScreen hidden;
    hidden.on_enter(fixture.context, splat::ScreenArgs{});
    CHECK(hidden.jobs().empty());
}

TEST_CASE("ui_queue_visibility a visible failed job still says so once") {
    // The control for the case above: the same row, with its album ticked, is
    // announced exactly as before stage H.
    Fixture fixture;
    fixture.enable({VisibilityKey::album("Family A")});
    JobView job;
    job.id = "j1";
    job.name = "hidden-source.jpg";
    job.photo = fixture.base / "data" / "Hidden" / "hidden-source.jpg";
    job.backend = "da2";
    job.album = "Family A";
    job.kind = "convert";
    job.state = "queued";
    job.source_folder = "folder:" + (fixture.base / "data" / "Hidden").generic_string();
    fixture.fake.inject_job(job);

    splat::ScenesScreen scenes;
    splat::ScreenArgs args;
    args.a = "Family A";
    scenes.on_enter(fixture.context, args);
    REQUIRE(scenes.pending() == 1);
    fixture.fake.set_job_state("j1", "failed", "boom");
    scenes.update(fixture.context, press(splat::Down));
    CHECK(fixture.has_toast("Could not convert hidden-source.jpg: boom"));
    CHECK(scenes.pending() == 0);
}

TEST_CASE("ui_queue_visibility the real job service refuses a hidden destination only") {
    splat_test::TempDir dir{"ui-queue-service"};
    // A runner with no backend: nothing is ever taken, so no thread starts work.
    splat::JobQueue queue = splat::JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    splat::JobRunner runner(
        queue, guard, [](const std::string&) { return nullptr; },
        splat::JobRunnerConfig{dir.path() / "scenes", dir.path() / "model.gguf", 64, 90});
    splat::JobService service(queue, runner);

    VisibilityContext context;
    context.scenes_root = dir.path() / "scenes";
    context.inbox = dir.path() / "data" / "ps5-splat-vr" / "inbox";
    context.mnt = dir.path() / "mnt";
    context.data = dir.path() / "data";
    context.app_data = dir.path() / "data" / "ps5-splat-vr";
    Visibility state = Visibility::all_visible(context);
    state.enabled = true;
    state.allowed = {VisibilityKey::album("Family A"),
                     VisibilityKey::console_folder(context.data / "Photos"),
                     VisibilityKey::location("smb0")};
    std::sort(state.allowed.begin(), state.allowed.end());
    splat::VisibilityScope scope(state);

    // A photo from an unticked console folder is accepted: the source half is
    // gone (stage U1), and its key still travels into meta.json.
    const splat::PhotoPick hidden(context.data / "Hidden" / "a.jpg", 0,
                                  "folder:" + (context.data / "Hidden").generic_string());
    REQUIRE(service.enqueue({hidden}, "da2", "Family A").ok());

    // A hidden album is a hidden destination.
    const splat::PhotoPick visible(context.data / "Photos" / "a.jpg", 0,
                                   "folder:" + (context.data / "Photos").generic_string());
    const Status refused_album = service.enqueue({visible}, "da2", "Family B");
    CHECK_FALSE(refused_album.ok());
    CHECK(refused_album.message() == splat::kNotVisibleText);

    // An import from an unticked folder is accepted; one into a hidden share is
    // not.
    REQUIRE(service.enqueue_import({context.data / "Hidden" / "scene.spz"}, "Family A", nullptr,
                                   Path(),
                                   "folder:" + (context.data / "Hidden").generic_string())
                .ok());
    const Status refused_share = service.enqueue_import(
        {context.data / "Photos" / "scene.spz"}, "Family A", nullptr, Path("smb1:/scenes"),
        "folder:" + (context.data / "Photos").generic_string());
    CHECK_FALSE(refused_share.ok());
    CHECK(refused_share.message() == splat::kNotVisibleText);

    // Both accepted rows are listed, each with the key the picker sent.
    const std::vector<JobView> accepted = service.snapshot();
    REQUIRE(accepted.size() == 2);
    CHECK(accepted[0].source_folder == "folder:" + (context.data / "Hidden").generic_string());
    CHECK(accepted[0].album == "Family A");
    CHECK(accepted[1].source_folder == "folder:" + (context.data / "Hidden").generic_string());

    // Hiding the destination does not delete the rows: the copy is filtered and
    // they come back when the album is ticked again.
    Visibility without_album = state;
    without_album.allowed = {VisibilityKey::location("smb0")};
    splat::set_visibility(without_album);
    CHECK(service.snapshot().empty());
    splat::set_visibility(state);
    CHECK(service.snapshot().size() == 2);

    // Turning the passcode off shows everything again.
    splat::set_visibility(Visibility::all_visible(context));
    CHECK(service.snapshot().size() == 2);
}

TEST_CASE("ui_queue_visibility a job row keeps its key through the fake service") {
    Fixture fixture;
    const splat::PhotoPick pick(fixture.base / "data" / "Photos" / "a.jpg", 0,
                                "folder:" + (fixture.base / "data" / "Photos").generic_string());
    REQUIRE(fixture.fake.enqueue({pick}, "da2", "Family A").ok());
    REQUIRE(fixture.fake.all_jobs().size() == 1);
    CHECK(fixture.fake.all_jobs()[0].source_folder == pick.source_folder);
    REQUIRE(fixture.fake.enqueue_import(
                {fixture.base / "data" / "Photos" / "scene.spz"}, "Family A", nullptr, Path(),
                "location:smb0")
                .ok());
    CHECK(fixture.fake.last_import_source() == "location:smb0");
    CHECK(fixture.fake.all_jobs()[1].source_folder == "location:smb0");
}
