// JobService: the real JobQueue + JobRunner behind IJobService, driven by its
// own worker thread. A fake reconstructor reports three progress steps, so the
// published views can be checked mid-job; it can also hold inside run() to test
// cancel and shutdown while a job is running.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/image.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "test_util.h"
#include "ui/job_service.h"

using namespace std::chrono_literals;

namespace {

using splat::JobService;
using splat::JobView;
using splat_test::release;
using splat_test::wait_until;
using splat_test::write_test_jpeg;

struct FakeState {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<int> runs{0};
    int steps = 3;
    bool block = false;     // hold inside run() until release()
    bool released = false;
};

// Reports three steps, so the service has something to publish; with block set
// it stops after the first one, which is what "the backend has no cancel point"
// looks like from the outside.
class StepReconstructor : public splat::IReconstructor {
public:
    explicit StepReconstructor(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    splat::BackendInfo info() const override { return {"fake", "test-only", 0}; }
    splat::Status load(const splat::Path&) override { return splat::Status::success(); }

    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput&,
                                         splat::Progress& progress) override {
        ++state_->runs;
        progress.report(1.0f / 3.0f, "step 1");
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->cv.notify_all();
            if (state_->block) {
                state_->cv.wait_for(lock, 10s, [this] { return state_->released; });
            }
        }
        progress.report(2.0f / 3.0f, "step 2");
        progress.report(1.0f, "step 3");
        return splat::Result<splat::SplatScene>::success(splat_test::make_test_scene(7));
    }

    void unload() override {}

private:
    std::shared_ptr<FakeState> state_;
};

const JobView* find_view(const std::vector<JobView>& views, const std::string& name) {
    for (const JobView& view : views) {
        if (view.name == name) return &view;
    }
    return nullptr;
}

bool any_running(const std::vector<JobView>& views) {
    for (const JobView& view : views) {
        if (view.state == "running") return true;
    }
    return false;
}

// The queue, the guard, the runner and the service a test drives.
struct Fixture {
    splat_test::TempDir dir{"ui-jobservice"};
    splat::JobQueue queue;
    splat::GpuPhaseGuard guard;
    std::shared_ptr<FakeState> state = std::make_shared<FakeState>();
    splat::JobRunner runner;
    std::atomic<int> changes{0};
    std::unique_ptr<JobService> service;

    Fixture()
        : queue(splat::JobQueue::open(dir.path() / "state" / "jobs.json").value()),
          runner(queue, guard,
                 [this](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
                     if (backend != "fake") return nullptr;
                     return std::make_unique<StepReconstructor>(state);
                 },
                 splat::JobRunnerConfig{dir.path() / "scenes", dir.path() / "model.gguf", 64, 90}) {}

    void start() {
        service = std::make_unique<JobService>(queue, runner, [this] { ++changes; });
    }

    splat::Path photo(const std::string& name) const { return dir.path() / name; }
};

}  // namespace

TEST_CASE("ui_jobservice publishes progress and finishes jobs in order") {
    Fixture fixture;
    fixture.state->block = true;  // hold at step 1 so the view can be read mid-job
    write_test_jpeg(fixture.photo("a.jpg"));
    write_test_jpeg(fixture.photo("b.jpg"));
    write_test_jpeg(fixture.photo("c.jpg"));
    fixture.start();

    REQUIRE(fixture.service->enqueue({fixture.photo("a.jpg")}, "fake", "2026-10-26").ok());
    // Mid-job the running row carries the backend's stage and its fraction.
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return views.size() == 1 && views[0].stage == "step 1";
    }));
    const std::vector<JobView> running = fixture.service->snapshot();
    REQUIRE(running.size() == 1);
    CHECK(running[0].name == "a.jpg");
    CHECK(running[0].backend == "fake");
    CHECK(running[0].album == "2026-10-26");
    CHECK(running[0].state == "running");
    CHECK(running[0].stage == "step 1");
    CHECK(running[0].fraction > 0.0f);
    CHECK(running[0].fraction < 1.0f);
    CHECK(fixture.service->busy());
    CHECK(fixture.changes.load() > 0);  // on_changed was called from the worker

    release(fixture.state);
    {
        std::lock_guard<std::mutex> lock(fixture.state->mutex);
        fixture.state->block = false;
    }
    REQUIRE(wait_until([&] { return !fixture.service->busy(); }));
    const std::vector<JobView> done = fixture.service->snapshot();
    REQUIRE(done.size() == 1);
    CHECK(done[0].state == "done");
    CHECK(done[0].message.empty());
    CHECK(std::filesystem::is_directory(fixture.dir.path() / "scenes" / "2026-10-26" / "a"));

    // Queued jobs run one at a time, oldest first.
    REQUIRE(fixture.service->enqueue({fixture.photo("b.jpg"), fixture.photo("c.jpg")}, "fake", "")
                .ok());
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return views.size() == 3 && views[2].state == "done";
    }));
    const std::vector<JobView> views = fixture.service->snapshot();
    REQUIRE(views.size() == 3);
    CHECK(views[0].name == "a.jpg");
    CHECK(views[1].name == "b.jpg");
    CHECK(views[2].name == "c.jpg");
    for (const JobView& view : views) CHECK(view.state == "done");
    CHECK(fixture.state->runs == 3);

    // The jobs are in the queue file, which is what survives a restart.
    const auto reloaded = splat::JobQueue::open(fixture.dir.path() / "state" / "jobs.json");
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    REQUIRE(reloaded.value().jobs().size() == 3);
    CHECK(reloaded.value().jobs()[0].album == "2026-10-26");
    CHECK(reloaded.value().jobs()[1].album.empty());
    CHECK(reloaded.value().jobs()[0].scene_folder ==
          fixture.dir.path() / "scenes" / "2026-10-26" / "a");
}

TEST_CASE("ui_jobservice cancels a queued job before it starts") {
    Fixture fixture;
    fixture.state->block = true;
    write_test_jpeg(fixture.photo("first.jpg"));
    write_test_jpeg(fixture.photo("second.jpg"));
    fixture.start();

    REQUIRE(fixture.service->enqueue({fixture.photo("first.jpg"), fixture.photo("second.jpg")},
                                     "fake", "")
                .ok());
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return views.size() == 2 && views[0].state == "running";
    }));

    const std::vector<JobView> views = fixture.service->snapshot();
    REQUIRE(views.size() == 2);
    CHECK(views[1].state == "queued");
    const splat::Status cancelled = fixture.service->cancel(views[1].id);
    REQUIRE_MESSAGE(cancelled.ok(), cancelled.message());

    const std::vector<JobView> after = fixture.service->snapshot();
    REQUIRE(after.size() == 2);
    CHECK(after[1].state == "cancelled");
    CHECK(after[1].message == "cancelled");
    CHECK(after[0].state == "running");

    release(fixture.state);
    REQUIRE(wait_until([&] { return !fixture.service->busy(); }));
    const std::vector<JobView> finished = fixture.service->snapshot();
    CHECK(finished[0].state == "done");
    CHECK(finished[1].state == "cancelled");
    CHECK(fixture.state->runs == 1);  // the cancelled job never ran
    CHECK_FALSE(std::filesystem::exists(fixture.dir.path() / "scenes" / "second"));
}

TEST_CASE("ui_jobservice cancels a running job after the photo in flight") {
    Fixture fixture;
    fixture.state->block = true;
    write_test_jpeg(fixture.photo("photo.jpg"));
    fixture.start();

    REQUIRE(fixture.service->enqueue({fixture.photo("photo.jpg")}, "fake", "").ok());
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return views.size() == 1 && views[0].state == "running" && views[0].stage == "step 1";
    }));

    const std::string id = fixture.service->snapshot()[0].id;
    REQUIRE(fixture.service->cancel(id).ok());
    // The view says the photo in flight still finishes.
    const std::vector<JobView> pending = fixture.service->snapshot();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].state == "running");
    CHECK(pending[0].message == "cancels after this photo");

    release(fixture.state);
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return !views.empty() && views[0].state == "cancelled";
    }));
    const std::vector<JobView> cancelled = fixture.service->snapshot();
    CHECK(cancelled[0].message == "cancelled");
    CHECK(fixture.state->runs == 1);
    // The runner stopped at the stage boundary and wrote no scene.
    CHECK_FALSE(std::filesystem::exists(fixture.dir.path() / "scenes" / "photo"));

    // Cancelling a terminal job is reported, not silently ignored.
    CHECK_FALSE(fixture.service->cancel(id).ok());
}

TEST_CASE("ui_jobservice joins the worker on shutdown and leaves the queue consistent") {
    Fixture fixture;
    fixture.state->block = true;
    write_test_jpeg(fixture.photo("photo.jpg"));
    fixture.start();

    REQUIRE(fixture.service->enqueue({fixture.photo("photo.jpg")}, "fake", "").ok());
    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        return views.size() == 1 && views[0].state == "running";
    }));
    const std::string id = fixture.service->snapshot()[0].id;

    // The destructor has to wait for the running job instead of abandoning it.
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        fixture.service.reset();
        destroyed.store(true);
    });
    std::this_thread::sleep_for(120ms);
    CHECK_FALSE(destroyed.load());

    release(fixture.state);
    destroyer.join();
    CHECK(destroyed.load());

    const auto reloaded = splat::JobQueue::open(fixture.dir.path() / "state" / "jobs.json");
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    const splat::Job* job = reloaded.value().find(id);
    REQUIRE(job != nullptr);
    // The job was allowed to finish, so it is recorded as done.
    CHECK(job->state == splat::JobState::Done);
    CHECK(fixture.state->runs == 1);
}

TEST_CASE("ui_jobservice snapshot is safe while jobs and readers run") {
    Fixture fixture;
    fixture.start();

    std::vector<splat::Path> photos;
    for (int index = 0; index < 6; ++index) {
        photos.push_back(write_test_jpeg(fixture.photo("stress_" + std::to_string(index) + ".jpg")));
    }
    REQUIRE(fixture.service->enqueue(photos, "fake", "").ok());

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<bool> bad_state{false};
    auto reader = [&] {
        while (!stop.load()) {
            const std::vector<JobView> views = fixture.service->snapshot();
            ++reads;
            if (views.size() > photos.size()) bad_state.store(true);
            for (const JobView& view : views) {
                const bool known = view.state == "queued" || view.state == "running" ||
                                   view.state == "done" || view.state == "failed" ||
                                   view.state == "cancelled";
                if (!known || view.fraction < 0.0f || view.fraction > 1.0f) {
                    bad_state.store(true);
                }
            }
        }
    };
    std::thread first(reader);
    std::thread second(reader);

    REQUIRE(wait_until([&] {
        const std::vector<JobView> views = fixture.service->snapshot();
        if (views.size() != photos.size()) return false;
        for (const JobView& view : views) {
            if (view.state != "done") return false;
        }
        return true;
    }, 60s));

    stop.store(true);
    first.join();
    second.join();
    CHECK(reads.load() > 0);
    CHECK_FALSE(bad_state.load());
    CHECK_FALSE(fixture.service->busy());

    const std::vector<JobView> views = fixture.service->snapshot();
    REQUIRE(views.size() == photos.size());
    for (const JobView& view : views) CHECK(view.state == "done");
    CHECK(find_view(views, "stress_0.jpg") != nullptr);
}
