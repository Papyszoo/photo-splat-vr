#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <thread>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/exif.h"
#include "common/image.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "scene/scene_folder.h"
#include "test_util.h"

using splat::JobQueue;

namespace {

struct FakeState {
    int loads = 0;
    int unloads = 0;
    int runs = 0;
    bool fail_load = false;
    bool fail_run = false;
    std::string fail_message = "fake backend failure";
    size_t splat_count = 7;
    int width = 0;
    int height = 0;
    float focal_px = 0.0f;
    bool inference_phase_during_run = false;
    bool viewing_blocked_during_run = false;
    splat::GpuPhaseGuard* guard = nullptr;
    std::function<void()> during_run;
};

class FakeReconstructor : public splat::IReconstructor {
public:
    explicit FakeReconstructor(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    splat::BackendInfo info() const override { return {"fake", "test-only", 0}; }

    splat::Status load(const splat::Path&) override {
        ++state_->loads;
        if (state_->fail_load) return splat::Status::error("weights are missing");
        return splat::Status::success();
    }

    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput& input,
                                         splat::Progress& progress) override {
        ++state_->runs;
        state_->width = input.rgb.width;
        state_->height = input.rgb.height;
        state_->focal_px = input.focal_px.value_or(0.0f);
        if (state_->guard != nullptr) {
            state_->inference_phase_during_run =
                state_->guard->phase() == splat::GpuPhase::Inference;
            state_->viewing_blocked_during_run = !state_->guard->try_begin_viewing();
            if (!state_->viewing_blocked_during_run) state_->guard->end_viewing();
        }
        progress.report(0.5f, "half way");
        if (state_->during_run) state_->during_run();
        if (state_->fail_run) {
            return splat::Result<splat::SplatScene>::failure(state_->fail_message);
        }
        return splat::Result<splat::SplatScene>::success(
            splat_test::make_test_scene(state_->splat_count));
    }

    void unload() override { ++state_->unloads; }

private:
    std::shared_ptr<FakeState> state_;
};

splat::Path write_test_jpeg(const splat::Path& path, int width, int height) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>(x * 255 / width);
            pixel[1] = static_cast<uint8_t>(y * 255 / height);
            pixel[2] = static_cast<uint8_t>((x + y) & 0xff);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(encoded.value().data()),
              static_cast<std::streamsize>(encoded.value().size()));
    REQUIRE(out.good());
    return path;
}

splat::JobRunner make_runner(splat::JobQueue& queue, splat::GpuPhaseGuard& guard,
                             const std::shared_ptr<FakeState>& state, const splat::Path& root,
                             int thumbnail_max_edge = 512) {
    return splat::JobRunner(
        queue, guard,
        [state](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
            if (backend != "fake") return nullptr;
            return std::make_unique<FakeReconstructor>(state);
        },
        splat::JobRunnerConfig{root / "scenes", root / "model.gguf", thumbnail_max_edge, 90});
}

}  // namespace

TEST_CASE("jobs: a full run writes a valid scene folder") {
    splat_test::TempDir dir("jobs-runner");
    const splat::Path queue_file = dir.path() / "state" / "jobs.json";
    JobQueue queue = JobQueue::open(queue_file).value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path(), 128);

    const splat::Path photo = write_test_jpeg(dir.path() / "photo.jpg", 640, 480);
    const auto id = queue.enqueue(photo, "fake");
    REQUIRE_MESSAGE(id.ok(), id.message());

    std::vector<std::pair<float, std::string>> progress;
    const auto result = runner.run_next([&progress](float fraction, const std::string& stage) {
        progress.push_back({fraction, stage});
    });
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());

    const splat::JobOutcome& outcome = result.value().value();
    CHECK(outcome.state == splat::JobState::Done);
    CHECK(outcome.message.empty());
    // The folder is named after the photo, not the job id.
    CHECK(outcome.scene_folder == dir.path() / "scenes" / "photo");
    CHECK(outcome.seconds_taken >= 0.0);
    REQUIRE(std::filesystem::is_directory(outcome.scene_folder));

    const auto loaded = splat::read_scene_folder(outcome.scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 7);
    CHECK(loaded.value().meta.backend == "fake");
    CHECK(loaded.value().meta.splat_count == 7);

    const auto expected_focal = splat::focal_px_from_exif(splat::ExifFocal{}, 640, 480);
    REQUIRE(expected_focal.ok());
    CHECK(loaded.value().meta.intrinsics.width == 640);
    CHECK(loaded.value().meta.intrinsics.height == 480);
    CHECK(loaded.value().meta.intrinsics.fx == doctest::Approx(expected_focal.value()));
    CHECK(loaded.value().meta.intrinsics.fy == doctest::Approx(expected_focal.value()));
    CHECK(loaded.value().meta.intrinsics.cx == doctest::Approx(320.0f));
    CHECK(loaded.value().meta.intrinsics.cy == doctest::Approx(240.0f));
    CHECK(loaded.value().meta.seconds_taken >= 0.0);

    const splat::SceneFiles files = splat::scene_files(outcome.scene_folder);
    CHECK(std::filesystem::file_size(files.original) == std::filesystem::file_size(photo));
    REQUIRE(std::filesystem::file_size(files.thumbnail) > 0);

    const auto thumbnail = splat::decode_image(files.thumbnail);
    REQUIRE_MESSAGE(thumbnail.ok(), thumbnail.message());
    CHECK(thumbnail.value().width == 128);
    CHECK(thumbnail.value().height == 96);

    CHECK(state->loads == 1);
    CHECK(state->unloads == 1);
    CHECK(state->runs == 1);
    CHECK(state->width == 640);
    CHECK(state->height == 480);
    CHECK(state->focal_px == doctest::Approx(expected_focal.value()));
    CHECK(state->inference_phase_during_run);
    CHECK(state->viewing_blocked_during_run);

    // The GPU is free again, and the model was reported unloaded first.
    CHECK(guard.phase() == splat::GpuPhase::Idle);
    CHECK_FALSE(guard.model_loaded());
    REQUIRE(guard.try_begin_viewing());
    guard.end_viewing();

    REQUIRE(!progress.empty());
    CHECK(progress.front().first == doctest::Approx(0.0f));
    CHECK(progress.back().first == doctest::Approx(1.0f));
    CHECK(progress.back().second == "done");
    for (size_t i = 1; i < progress.size(); ++i) {
        CHECK(progress[i].first >= progress[i - 1].first);
    }

    const auto reloaded = JobQueue::open(queue_file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    const splat::Job* stored = reloaded.value().find(id.value());
    REQUIRE(stored != nullptr);
    CHECK(stored->state == splat::JobState::Done);
    CHECK(stored->scene_folder == outcome.scene_folder);
}


TEST_CASE("jobs: a runner released by shutdown does not leave the job running") {
    // A viewer holds the GPU, so run_next blocks in begin_inference(). Closing
    // the app must release it: the process cannot hang on a parked thread, and
    // the queue must not keep a Running job forever.
    splat_test::TempDir dir("jobs-runner-shutdown");
    const splat::Path queue_file = dir.path() / "state" / "jobs.json";
    JobQueue queue = JobQueue::open(queue_file).value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const splat::Path photo = write_test_jpeg(dir.path() / "photo.jpg", 64, 48);
    REQUIRE_MESSAGE(queue.enqueue(photo, "fake").ok(), "enqueue");
    REQUIRE(guard.try_begin_viewing());

    std::promise<void> started;
    std::future<void> started_future = started.get_future();
    splat::Result<std::optional<splat::JobOutcome>> result =
        splat::Result<std::optional<splat::JobOutcome>>::failure("not run");
    std::thread worker([&] {
        started.set_value();
        result = runner.run_next();
    });
    REQUIRE(started_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(result.message() == "not run");  // still parked in begin_inference()

    guard.shutdown();
    worker.join();
    guard.end_viewing();

    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    // The job was taken and then interrupted, so it is recorded as failed with
    // an explanation instead of staying Running until the next restart.
    CHECK(result.value()->state == splat::JobState::Failed);
    CHECK(result.value()->message.find("shutting down") != std::string::npos);
    CHECK(state->loads == 0);  // no backend work happened
}

TEST_CASE("jobs: a backend failure is recorded on the job with a message") {
    splat_test::TempDir dir("jobs-fail");
    const splat::Path queue_file = dir.path() / "state" / "jobs.json";
    JobQueue queue = JobQueue::open(queue_file).value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    state->fail_run = true;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const splat::Path photo = write_test_jpeg(dir.path() / "photo.jpg", 64, 48);
    const auto id = queue.enqueue(photo, "fake");
    REQUIRE(id.ok());

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    const splat::JobOutcome& outcome = result.value().value();
    CHECK(outcome.state == splat::JobState::Failed);
    CHECK(outcome.message == "reconstruction failed: fake backend failure");
    CHECK_FALSE(std::filesystem::exists(dir.path() / "scenes" / "photo"));

    // The backend is unloaded even on failure, so viewing may start.
    CHECK(state->runs == 1);
    CHECK(state->unloads == 1);
    CHECK(guard.phase() == splat::GpuPhase::Idle);
    CHECK_FALSE(guard.model_loaded());
    REQUIRE(guard.try_begin_viewing());
    guard.end_viewing();

    const auto reloaded = JobQueue::open(queue_file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    const splat::Job* stored = reloaded.value().find(id.value());
    REQUIRE(stored != nullptr);
    CHECK(stored->state == splat::JobState::Failed);
    CHECK(stored->message == outcome.message);
}

TEST_CASE("jobs: a load failure fails the job without running inference") {
    splat_test::TempDir dir("jobs-loadfail");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    state->fail_load = true;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const splat::Path photo = write_test_jpeg(dir.path() / "photo.jpg", 32, 32);
    REQUIRE(queue.enqueue(photo, "fake").ok());

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Failed);
    CHECK(result.value().value().message.find("cannot load backend") != std::string::npos);
    CHECK(state->runs == 0);
    CHECK(state->unloads == 0);  // nothing was loaded
    CHECK_FALSE(guard.model_loaded());
    CHECK(guard.phase() == splat::GpuPhase::Idle);
}

TEST_CASE("jobs: a missing photo fails the job instead of throwing") {
    splat_test::TempDir dir("jobs-missing");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const auto id = queue.enqueue(dir.path() / "not-there.jpg", "fake");
    REQUIRE(id.ok());

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Failed);
    CHECK(result.value().value().message.find("cannot read photo") != std::string::npos);
    CHECK(state->unloads == 1);  // the backend was loaded, then unloaded
    CHECK(guard.phase() == splat::GpuPhase::Idle);
}

TEST_CASE("jobs: an unknown backend fails the job with a message") {
    splat_test::TempDir dir("jobs-backend");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const auto id = queue.enqueue(write_test_jpeg(dir.path() / "photo.jpg", 32, 32), "sharp");
    REQUIRE(id.ok());

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Failed);
    CHECK(result.value().value().message.find("no reconstruction backend") != std::string::npos);
    CHECK(state->loads == 0);
}

TEST_CASE("jobs: the runner processes queued jobs one at a time") {
    splat_test::TempDir dir("jobs-serial-run");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const auto first = queue.enqueue(write_test_jpeg(dir.path() / "a.jpg", 32, 32), "fake");
    const auto second = queue.enqueue(write_test_jpeg(dir.path() / "b.jpg", 32, 32), "fake");
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    const auto first_run = runner.run_next();
    REQUIRE(first_run.ok());
    REQUIRE(first_run.value().has_value());
    CHECK(first_run.value().value().job_id == first.value());
    CHECK(state->runs == 1);
    REQUIRE(queue.find(second.value()) != nullptr);
    CHECK(queue.find(second.value())->state == splat::JobState::Queued);

    const auto second_run = runner.run_next();
    REQUIRE(second_run.ok());
    REQUIRE(second_run.value().has_value());
    CHECK(second_run.value().value().job_id == second.value());
    CHECK(state->runs == 2);

    const auto idle = runner.run_next();
    REQUIRE(idle.ok());
    CHECK_FALSE(idle.value().has_value());
    CHECK(state->runs == 2);
}

TEST_CASE("jobs: cancelling during inference keeps the job cancelled and writes no scene") {
    splat_test::TempDir dir("jobs-cancel-run");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const auto id = queue.enqueue(write_test_jpeg(dir.path() / "photo.jpg", 32, 32), "fake");
    REQUIRE(id.ok());
    state->during_run = [&queue, &id] { REQUIRE_MESSAGE(queue.cancel(id.value()).ok(), "cancel"); };

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Cancelled);
    CHECK(result.value().value().message == "cancelled");
    CHECK_FALSE(std::filesystem::exists(dir.path() / "scenes" / "photo"));
    CHECK(state->unloads == 1);
    CHECK(guard.phase() == splat::GpuPhase::Idle);
    CHECK_FALSE(guard.model_loaded());
}

TEST_CASE("jobs: a cancel request stops the job at the next stage boundary") {
    // What JobService's Square does: another thread asks, the runner thread
    // records the cancellation, and no scene folder is written.
    splat_test::TempDir dir("jobs-cancel-request");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const auto id = queue.enqueue(write_test_jpeg(dir.path() / "photo.jpg", 32, 32), "fake");
    REQUIRE(id.ok());
    bool requested = false;
    runner.set_cancel_request([&requested](const std::string& job_id) {
        return requested && job_id == "job-000001";
    });
    // The request arrives while the backend runs, so the runner sees it at its
    // first checkpoint after inference.
    state->during_run = [&requested] { requested = true; };

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Cancelled);
    CHECK(result.value().value().message == "cancelled");
    CHECK_FALSE(std::filesystem::exists(dir.path() / "scenes" / "photo"));
    CHECK(state->unloads == 1);
    CHECK(guard.phase() == splat::GpuPhase::Idle);

    // A stale request for the job that already finished cancels nothing.
    requested = true;
    const auto second = queue.enqueue(write_test_jpeg(dir.path() / "other.jpg", 32, 32), "fake");
    REQUIRE(second.ok());
    const auto second_run = runner.run_next();
    REQUIRE(second_run.ok());
    REQUIRE(second_run.value().has_value());
    CHECK(second_run.value().value().state == splat::JobState::Done);
    CHECK(std::filesystem::is_directory(second_run.value().value().scene_folder));
}

TEST_CASE("jobs: a job lands in its album with the photo stem as folder name") {
    splat_test::TempDir dir("jobs-album");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    std::filesystem::create_directories(dir.path() / "inbox");
    const splat::Path photo = write_test_jpeg(dir.path() / "inbox" / "KITCHEN 1.jpg", 32, 32);
    const auto id = queue.enqueue(photo, "fake", "2026-10-26");
    REQUIRE_MESSAGE(id.ok(), id.message());

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    const splat::JobOutcome& outcome = result.value().value();
    CHECK(outcome.state == splat::JobState::Done);
    // Space is not in [A-Za-z0-9._-] and becomes '_'.
    CHECK(outcome.scene_folder == dir.path() / "scenes" / "2026-10-26" / "KITCHEN_1");
    CHECK(std::filesystem::is_directory(outcome.scene_folder));

    const auto loaded = splat::read_scene_folder(outcome.scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.source == "inbox");

    // "" keeps the scene at the root of the output folder.
    const splat::Path root_photo = write_test_jpeg(dir.path() / "loose.png.jpg", 32, 32);
    REQUIRE(queue.enqueue(root_photo, "fake").ok());
    const auto root_run = runner.run_next();
    REQUIRE(root_run.ok());
    REQUIRE(root_run.value().has_value());
    CHECK(root_run.value().value().scene_folder == dir.path() / "scenes" / "loose.png");
}

TEST_CASE("jobs: a repeated photo name gets -2 and -3 folders, and long names are cut to 48") {
    splat_test::TempDir dir("jobs-collision");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    for (const char* folder : {"one", "two", "three"}) {
        std::filesystem::create_directories(dir.path() / folder);
        REQUIRE(queue.enqueue(write_test_jpeg(dir.path() / folder / "photo.jpg", 32, 32), "fake").ok());
    }

    std::vector<splat::Path> folders;
    for (int run = 0; run < 3; ++run) {
        const auto result = runner.run_next();
        REQUIRE_MESSAGE(result.ok(), result.message());
        REQUIRE(result.value().has_value());
        REQUIRE(result.value().value().state == splat::JobState::Done);
        folders.push_back(result.value().value().scene_folder);
    }
    CHECK(folders[0] == dir.path() / "scenes" / "photo");
    CHECK(folders[1] == dir.path() / "scenes" / "photo-2");
    CHECK(folders[2] == dir.path() / "scenes" / "photo-3");
    for (const splat::Path& folder : folders) CHECK(std::filesystem::is_directory(folder));

    // 60 'a's: the folder keeps 48 of them.
    std::filesystem::create_directories(dir.path() / "long");
    const std::string long_stem(60, 'a');
    REQUIRE(queue.enqueue(write_test_jpeg(dir.path() / "long" / (long_stem + ".jpg"), 32, 32), "fake").ok());
    const auto long_run = runner.run_next();
    REQUIRE(long_run.ok());
    REQUIRE(long_run.value().has_value());
    const std::string folder = long_run.value().value().scene_folder.filename().string();
    CHECK(folder.size() == 48);
    CHECK(folder == std::string(48, 'a'));

    // The sanitizer never produces an empty or hidden folder name.
    CHECK(splat::scene_folder_base(splat::Path("/a/holiday photo!.JPG")) == "holiday_photo_");
    CHECK(splat::scene_folder_base(splat::Path("/a/.hidden.jpg")) == "_hidden");
    CHECK(splat::scene_folder_base(splat::Path("/a/....jpg")) == "_..");
    CHECK(splat::scene_folder_base(splat::Path("")) == "photo");
}
