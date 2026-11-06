#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <thread>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/exif.h"
#include "common/image.h"
#include "gallery/photo_turns.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "scene/ply.h"
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

std::vector<uint8_t> encode_test_jpeg(int width, int height) {
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
    return encoded.value();
}

splat::Path write_bytes(const splat::Path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
    return path;
}

splat::Path write_test_jpeg(const splat::Path& path, int width, int height) {
    return write_bytes(path, encode_test_jpeg(width, height));
}

std::vector<uint8_t> read_bytes(const splat::Path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// The JPEG plus an Exif APP1 segment right after SOI with one IFD0 entry:
// Orientation = `orientation`. This is what a portrait phone photo carries.
std::vector<uint8_t> with_exif_orientation(const std::vector<uint8_t>& jpeg, int orientation) {
    std::vector<uint8_t> tiff = {'I', 'I', 0x2A, 0x00, 8, 0, 0, 0};  // II, 42, IFD0 at 8
    tiff.push_back(1);  // one entry
    tiff.push_back(0);
    tiff.push_back(0x12);  // tag 0x0112 Orientation
    tiff.push_back(0x01);
    tiff.push_back(3);  // SHORT
    tiff.push_back(0);
    tiff.push_back(1);  // count 1
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(static_cast<uint8_t>(orientation));
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);  // no next IFD
    tiff.push_back(0);
    tiff.push_back(0);
    tiff.push_back(0);

    std::vector<uint8_t> app1 = {0xFF, 0xE1};
    const auto length = static_cast<uint16_t>(2 + 6 + tiff.size());
    app1.push_back(static_cast<uint8_t>(length >> 8));
    app1.push_back(static_cast<uint8_t>(length));
    const char exif_header[] = {'E', 'x', 'i', 'f', 0, 0};
    app1.insert(app1.end(), exif_header, exif_header + 6);
    app1.insert(app1.end(), tiff.begin(), tiff.end());

    std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
    out.insert(out.end(), app1.begin(), app1.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
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

TEST_CASE("jobs: the runner writes the job's source folder into meta.json") {
    splat_test::TempDir dir("jobs-source-folder");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    std::filesystem::create_directories(dir.path() / "inbox");
    const splat::Path photo = write_test_jpeg(dir.path() / "inbox" / "a.jpg", 32, 32);
    // The picker's key travels through the queue into the scene's meta.json, so
    // hiding the source folder later hides the scene wherever it now lives.
    const auto converted = queue.enqueue(photo, "fake", "Family A", 0, splat::Path(),
                                        "folder:/data/Photos");
    REQUIRE_MESSAGE(converted.ok(), converted.message());
    const auto run = runner.run_next();
    REQUIRE(run.ok());
    REQUIRE(run.value().has_value());
    const auto scene = splat::read_scene_folder(run.value()->scene_folder);
    REQUIRE(scene.ok());
    CHECK(scene.value().meta.source_folder == "folder:/data/Photos");

    // The import path writes it too.
    const splat::SplatScene source = splat_test::make_test_scene(4);
    REQUIRE(splat::export_ply(dir.path() / "inbox" / "cloud.ply", source).ok());
    const auto imported = queue.enqueue_import(dir.path() / "inbox" / "cloud.ply", "Family A",
                                               splat::Path(), "source:usb0");
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    const auto imported_run = runner.run_next();
    REQUIRE(imported_run.ok());
    REQUIRE(imported_run.value().has_value());
    REQUIRE(imported_run.value()->state == splat::JobState::Done);
    const auto imported_scene = splat::read_scene_folder(imported_run.value()->scene_folder);
    REQUIRE(imported_scene.ok());
    CHECK(imported_scene.value().meta.source_folder == "source:usb0");
    CHECK(imported_scene.value().meta.origin_present);
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

TEST_CASE("jobs: an EXIF orientation 6 photo writes the swapped dimensions") {
    splat_test::TempDir dir("jobs-orientation");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    // 64x32 as stored, orientation 6 (rotate 90 clockwise on display).
    const std::vector<uint8_t> file = with_exif_orientation(encode_test_jpeg(64, 32), 6);
    const splat::Path photo = write_bytes(dir.path() / "portrait.jpg", file);
    REQUIRE_MESSAGE(queue.enqueue(photo, "fake").ok(), "enqueue");

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    const splat::JobOutcome& outcome = result.value().value();
    REQUIRE(outcome.state == splat::JobState::Done);

    // The scene and the backend saw the oriented image, not the stored one.
    CHECK(state->width == 32);
    CHECK(state->height == 64);

    const auto loaded = splat::read_scene_folder(outcome.scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    // cx' = (source height - 1) - cy, cy' = cx: the same map as the pixels.
    CHECK(loaded.value().meta.intrinsics.cx == doctest::Approx(15.0f));
    CHECK(loaded.value().meta.intrinsics.cy == doctest::Approx(32.0f));
    CHECK(loaded.value().meta.intrinsics.fx == doctest::Approx(loaded.value().meta.intrinsics.fy));

    // An oriented JPEG is not the file's own pixels: it is re-encoded, so the
    // stored original.jpg must differ from the bytes that were read.
    const splat::SceneFiles files = splat::scene_files(outcome.scene_folder);
    CHECK(read_bytes(files.original) != file);
    const auto original = splat::decode_image(files.original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
}

TEST_CASE("jobs: manual quarter turns rotate the scene and rewrite original.jpg") {
    splat_test::TempDir dir("jobs-rotate");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const std::vector<uint8_t> file = encode_test_jpeg(64, 32);
    const splat::Path photo = write_bytes(dir.path() / "sideways.jpg", file);
    REQUIRE_MESSAGE(queue.enqueue(photo, "fake", "", 1).ok(), "enqueue");

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    REQUIRE(result.value().value().state == splat::JobState::Done);

    CHECK(state->width == 32);
    CHECK(state->height == 64);

    const auto loaded = splat::read_scene_folder(result.value().value().scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    CHECK(loaded.value().meta.intrinsics.cx == doctest::Approx(15.0f));
    CHECK(loaded.value().meta.intrinsics.cy == doctest::Approx(32.0f));

    const splat::SceneFiles files = splat::scene_files(result.value().value().scene_folder);
    CHECK(read_bytes(files.original) != file);
    const auto original = splat::decode_image(files.original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
}

TEST_CASE("jobs: EXIF orientation 6 is applied before the manual quarter turn") {
    splat_test::TempDir dir("jobs-orientation-rotate");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    // 64x32 as stored, EXIF orientation 6 (90 clockwise on display). The manual
    // quarter turn is applied after it, exactly the order decode_photo() and
    // orient_intrinsics() document: 64x32 -> 32x64 -> 64x32.
    const std::vector<uint8_t> file = with_exif_orientation(encode_test_jpeg(64, 32), 6);
    const splat::Path photo = write_bytes(dir.path() / "portrait-turned.jpg", file);
    REQUIRE_MESSAGE(queue.enqueue(photo, "fake", "", 1).ok(), "enqueue");

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    REQUIRE(result.value().value().state == splat::JobState::Done);

    // The backend saw EXIF first and then the turn: the stored 64x32 is portrait
    // after orientation 6, and the turn brings it back to landscape.
    CHECK(state->width == 64);
    CHECK(state->height == 32);

    const auto loaded = splat::read_scene_folder(result.value().value().scene_folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 64);
    CHECK(loaded.value().meta.intrinsics.height == 32);
    // Source intrinsics were cx 32 / cy 16. EXIF 6 maps them to cx 15 / cy 32;
    // the clockwise quarter then maps that to cx 63-32 = 31 / cy 15.
    CHECK(loaded.value().meta.intrinsics.cx == doctest::Approx(31.0f));
    CHECK(loaded.value().meta.intrinsics.cy == doctest::Approx(15.0f));
    CHECK(loaded.value().meta.intrinsics.fx ==
          doctest::Approx(loaded.value().meta.intrinsics.fy));

    // The written original.jpg is the fully oriented and turned image.
    const splat::SceneFiles files = splat::scene_files(result.value().value().scene_folder);
    CHECK(read_bytes(files.original) != file);
    const auto original = splat::decode_image(files.original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 64);
    CHECK(original.value().height == 32);
}

TEST_CASE("jobs: a turned photo converts with exactly one turn on both routes") {
    splat_test::TempDir dir("jobs-photo-turn");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());
    splat::PhotoTurnStore turns(dir.path() / "state");

    SUBCASE("the source's Orientation tag was rewritten") {
        // 64x32 stored, a right turn rewrites the file to EXIF 6. The job is
        // enqueued with the store's turn (0), so the file's tag is the only
        // turn: 32x64, not the 64x32 a second turn would produce.
        const splat::Path photo =
            write_bytes(dir.path() / "rewritten.jpg", encode_test_jpeg(64, 32));
        const auto turned = splat::rotate_photo_file(photo, 1, turns, true);
        REQUIRE_MESSAGE(turned.ok(), turned.message());
        REQUIRE(turned.value().route == splat::PhotoTurnResult::Route::rewritten);
        REQUIRE_MESSAGE(queue.enqueue(photo, "fake", "", turns.load(photo)).ok(), "enqueue");
        const auto result = runner.run_next();
        REQUIRE_MESSAGE(result.ok(), result.message());
        REQUIRE(result.value().has_value());
        REQUIRE(result.value().value().state == splat::JobState::Done);
        CHECK(state->width == 32);
        CHECK(state->height == 64);
        const auto loaded = splat::read_scene_folder(result.value().value().scene_folder);
        REQUIRE_MESSAGE(loaded.ok(), loaded.message());
        CHECK(loaded.value().meta.intrinsics.width == 32);
        CHECK(loaded.value().meta.intrinsics.height == 64);
        const auto original = splat::decode_image(splat::scene_files(
            result.value().value().scene_folder).original);
        REQUIRE_MESSAGE(original.ok(), original.message());
        CHECK(original.value().width == 32);
        CHECK(original.value().height == 64);
    }

    SUBCASE("the turn was saved because the file cannot be rewritten") {
        // A PNG: the source is untouched and the quarter goes to the job.
        std::vector<uint8_t> rgba(64 * 32 * 4, 128);
        const auto encoded = splat::encode_png(rgba.data(), 64, 32);
        REQUIRE_MESSAGE(encoded.ok(), encoded.message());
        const splat::Path photo = write_bytes(dir.path() / "saved.png", encoded.value());
        const auto turned = splat::rotate_photo_file(photo, 1, turns, true);
        REQUIRE_MESSAGE(turned.ok(), turned.message());
        REQUIRE(turned.value().route == splat::PhotoTurnResult::Route::saved);
        REQUIRE(turned.value().quarters == 1);
        CHECK(read_bytes(photo) == encoded.value());
        REQUIRE_MESSAGE(queue.enqueue(photo, "fake", "", turns.load(photo)).ok(), "enqueue");
        const auto result = runner.run_next();
        REQUIRE_MESSAGE(result.ok(), result.message());
        REQUIRE(result.value().has_value());
        REQUIRE(result.value().value().state == splat::JobState::Done);
        CHECK(state->width == 32);
        CHECK(state->height == 64);
        const auto loaded = splat::read_scene_folder(result.value().value().scene_folder);
        REQUIRE_MESSAGE(loaded.ok(), loaded.message());
        CHECK(loaded.value().meta.intrinsics.width == 32);
        CHECK(loaded.value().meta.intrinsics.height == 64);
    }
}

TEST_CASE("jobs: a scene turn runs without a model and rewrites the folder") {
    splat_test::TempDir dir("jobs-rotate-run");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    // A 64x32 scene folder, written the way a conversion writes one.
    const splat::SplatScene scene = splat_test::make_test_scene(8);
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 64;
    meta.intrinsics.height = 32;
    meta.intrinsics.fx = 80.0f;
    meta.intrinsics.fy = 80.0f;
    meta.intrinsics.cx = 32.0f;
    meta.intrinsics.cy = 16.0f;
    const std::vector<uint8_t> jpeg = encode_test_jpeg(64, 32);
    const splat::Path folder = dir.path() / "scenes" / "s1";
    REQUIRE_MESSAGE(
        splat::write_scene_folder(folder, scene, meta, jpeg, jpeg).ok(), "write scene");

    const auto id = queue.enqueue_rotate(folder, 1);
    REQUIRE_MESSAGE(id.ok(), id.message());
    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Done);
    CHECK(result.value().value().scene_folder == folder);

    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 32);
    CHECK(loaded.value().meta.intrinsics.height == 64);
    const auto original = splat::decode_image(splat::scene_files(folder).original);
    REQUIRE_MESSAGE(original.ok(), original.message());
    CHECK(original.value().width == 32);
    CHECK(original.value().height == 64);
    // No model was loaded for a turn.
    CHECK(state->loads == 0);
}

TEST_CASE("jobs: a scene turn is refused while the viewer holds that scene") {
    splat_test::TempDir dir("jobs-rotate-open");
    JobQueue queue = JobQueue::open(dir.path() / "state" / "jobs.json").value();
    splat::GpuPhaseGuard guard;
    auto state = std::make_shared<FakeState>();
    state->guard = &guard;
    splat::JobRunner runner = make_runner(queue, guard, state, dir.path());

    const splat::SplatScene scene = splat_test::make_test_scene(8);
    splat::SceneMeta meta;
    meta.backend = "sharp";
    meta.intrinsics.width = 64;
    meta.intrinsics.height = 32;
    meta.intrinsics.fx = 80.0f;
    meta.intrinsics.fy = 80.0f;
    meta.intrinsics.cx = 32.0f;
    meta.intrinsics.cy = 16.0f;
    const std::vector<uint8_t> jpeg = encode_test_jpeg(64, 32);
    const splat::Path folder = dir.path() / "scenes" / "s1";
    REQUIRE_MESSAGE(
        splat::write_scene_folder(folder, scene, meta, jpeg, jpeg).ok(), "write scene");

    // The turn is queued, then the user opens the scene (it sat behind another
    // job). The runner must not rewrite the folder under the renderer.
    REQUIRE_MESSAGE(queue.enqueue_rotate(folder, 1).ok(), "enqueue");
    runner.set_scene_open(
        [folder](const splat::Path& open) { return open.lexically_normal() == folder; });

    const auto result = runner.run_next();
    REQUIRE_MESSAGE(result.ok(), result.message());
    REQUIRE(result.value().has_value());
    CHECK(result.value().value().state == splat::JobState::Failed);
    CHECK(result.value().value().message.find("close the scene") != std::string::npos);

    // The scene the viewer holds is untouched.
    const auto loaded = splat::read_scene_folder(folder);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().meta.intrinsics.width == 64);
    CHECK(loaded.value().meta.intrinsics.height == 32);
    CHECK(state->loads == 0);
}
