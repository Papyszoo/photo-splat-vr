#include "jobs/job_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/progress.h"
#include "jobs/image_io.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

namespace splat {
namespace {

// Longest folder name taken from the photo stem. A collision suffix is added
// after this cut, so a folder name can be 50 characters long.
constexpr size_t kMaxSceneFolderName = 48;

// Holds the GPU for inference and releases it on every return path. begin()
// fails only when the guard has been shut down; acquired() then tells the
// caller whether the job ran at all.
class ScopedInference {
public:
    explicit ScopedInference(GpuPhaseGuard& guard) : guard_(guard) {
        acquired_ = guard_.begin_inference();
    }
    ~ScopedInference() {
        if (acquired_) guard_.end_inference();
    }

    bool acquired() const { return acquired_; }

    ScopedInference(const ScopedInference&) = delete;
    ScopedInference& operator=(const ScopedInference&) = delete;

private:
    GpuPhaseGuard& guard_;
    bool acquired_ = false;
};

// Loads the backend and guarantees unload before inference ends, so the guard
// never admits a viewer while weights are resident.
class LoadedBackend {
public:
    explicit LoadedBackend(GpuPhaseGuard& guard) : guard_(guard) {}
    ~LoadedBackend() { unload(); }

    LoadedBackend(const LoadedBackend&) = delete;
    LoadedBackend& operator=(const LoadedBackend&) = delete;

    Status load(IReconstructor& reconstructor, const Path& model) {
        reconstructor_ = &reconstructor;
        const Status status = reconstructor.load(model);
        if (!status.ok()) {
            reconstructor_ = nullptr;
            return status;
        }
        guard_.set_model_loaded(true);
        return Status::success();
    }

    void unload() {
        if (reconstructor_ == nullptr) return;
        reconstructor_->unload();
        reconstructor_ = nullptr;
        guard_.set_model_loaded(false);
    }

private:
    GpuPhaseGuard& guard_;
    IReconstructor* reconstructor_ = nullptr;
};

// Borrows the queue for one short operation. The mutex is optional: without one
// the runner thread owns the queue alone (app mode, the CLIs and the tests that
// drive run_next directly).
class QueueAccess {
public:
    explicit QueueAccess(std::mutex* mutex) {
        if (mutex != nullptr) lock_ = std::unique_lock<std::mutex>(*mutex);
    }

    QueueAccess(const QueueAccess&) = delete;
    QueueAccess& operator=(const QueueAccess&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
};

JobOutcome outcome_from_queue(const JobQueue& queue, const std::string& id) {
    JobOutcome outcome;
    outcome.job_id = id;
    outcome.state = JobState::Failed;
    if (const Job* job = queue.find(id)) {
        outcome.state = job->state;
        outcome.message = job->message;
        outcome.scene_folder = job->scene_folder;
        outcome.seconds_taken = job->seconds_taken;
    }
    return outcome;
}

bool path_exists(const Path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

// output_root/<album>, created on demand by write_scene_folder.
Path album_root(const Path& output_root, const std::string& album) {
    return album.empty() ? output_root : output_root / album;
}

// `base`, else `base-2`, `base-3`, ...: the same rule default_album_name() uses,
// so two conversions of the same photo name never share a folder.
Path unique_scene_folder(const Path& root, const std::string& base) {
    const Path first = root / base;
    if (!path_exists(first)) return first;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const Path candidate = root / (base + "-" + std::to_string(suffix));
        if (!path_exists(candidate)) return candidate;
    }
    return root / (base + "-1000");
}

// The photo's parent folder name is the source label the runner can know
// without the gallery (inbox, usb0, PS5 screenshots, ...). A photo directly at
// a filesystem root has no parent name and gets an empty source.
std::string source_label(const Path& photo) { return photo.parent_path().filename().string(); }

}  // namespace

std::string scene_folder_base(const Path& photo) {
    const std::string stem = photo.stem().string();
    std::string name;
    name.reserve(std::min(stem.size(), kMaxSceneFolderName));
    for (const char c : stem) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        name.push_back(keep ? c : '_');
        if (name.size() == kMaxSceneFolderName) break;
    }
    if (name.empty()) return "photo";
    // A hidden folder would disappear from the gallery listing, and "." or ".."
    // would escape the album.
    if (name.front() == '.') name.front() = '_';
    return name;
}

JobRunner::JobRunner(JobQueue& queue, GpuPhaseGuard& guard, Factory factory, JobRunnerConfig config)
    : queue_(queue), guard_(guard), factory_(std::move(factory)), config_(std::move(config)) {}

void JobRunner::set_cancel_request(std::function<bool(const std::string& id)> request) {
    cancel_request_ = std::move(request);
}

void JobRunner::set_queue_mutex(std::mutex* mutex) { queue_mutex_ = mutex; }

Result<std::optional<JobOutcome>> JobRunner::run_next(const ProgressCallback& progress) {
    using Outcome = Result<std::optional<JobOutcome>>;

    if (!factory_) return Outcome::failure("no reconstruction backend factory configured");
    if (config_.output_root.empty()) return Outcome::failure("scene output root is not configured");
    if (guard_.stopped()) return Outcome::failure("the GPU phase guard is shutting down");

    Result<std::optional<Job>> taken = [&]() -> Result<std::optional<Job>> {
        QueueAccess access(queue_mutex_);
        return queue_.take_next();
    }();
    if (!taken.ok()) return Outcome::failure(taken.message());
    if (!taken.value()) return Outcome::success(std::nullopt);

    const Job job = taken.value().value();
    const auto started = std::chrono::steady_clock::now();
    Progress reporter(progress);
    reporter.report(0.0f, "starting");

    ScopedInference inference(guard_);

    auto finish_failed = [&](const std::string& detail) -> Outcome {
        Status recorded = Status::success();
        JobOutcome outcome;
        {
            QueueAccess access(queue_mutex_);
            const Job* current = queue_.find(job.id);
            // A job cancelled while this stage ran keeps its state and message.
            if (current != nullptr && current->state != JobState::Cancelled) {
                recorded = queue_.mark_failed(job.id, detail);
            }
            outcome = outcome_from_queue(queue_, job.id);
        }
        if (!recorded.ok()) return Outcome::failure(recorded.message());
        reporter.report(1.0f, "failed");
        return Outcome::success(std::optional<JobOutcome>(std::move(outcome)));
    };

    // Closing down while waiting for a viewer is not a failed conversion, but
    // the job cannot stay Running either: record it and let the caller decide.
    if (!inference.acquired()) {
        return finish_failed(
            "interrupted: the GPU phase guard is shutting down, so this job did not run");
    }

    // The UI can only ask for a cancel between stages: the queue stays owned by
    // this thread, so the request is applied here, not on the caller's thread.
    auto cancel_requested = [&]() -> bool {
        const bool requested = cancel_request_ && cancel_request_(job.id);
        QueueAccess access(queue_mutex_);
        const Job* current = queue_.find(job.id);
        if (requested && current != nullptr && current->state == JobState::Running) {
            (void)queue_.cancel(job.id);
            current = queue_.find(job.id);
        }
        return current != nullptr && current->state == JobState::Cancelled;
    };

    auto finish_cancelled = [&]() -> Outcome {
        reporter.report(1.0f, "cancelled");
        JobOutcome outcome;
        {
            QueueAccess access(queue_mutex_);
            outcome = outcome_from_queue(queue_, job.id);
        }
        return Outcome::success(std::optional<JobOutcome>(std::move(outcome)));
    };

    if (cancel_requested()) return finish_cancelled();

    std::unique_ptr<IReconstructor> reconstructor = factory_(job.backend);
    if (!reconstructor) {
        return finish_failed("no reconstruction backend registered for '" + job.backend + "'");
    }

    LoadedBackend backend(guard_);
    const Status loaded = backend.load(*reconstructor, config_.model_path);
    if (!loaded.ok()) {
        return finish_failed("cannot load backend '" + job.backend + "': " + loaded.message());
    }
    if (cancel_requested()) return finish_cancelled();

    Result<std::vector<uint8_t>> read = read_file_bytes(job.photo);
    if (!read.ok()) return finish_failed("cannot read photo: " + read.message());
    std::vector<uint8_t> file_bytes = read.take();

    Result<Image> decoded = decode_image(file_bytes.data(), file_bytes.size());
    if (!decoded.ok()) return finish_failed("cannot decode photo: " + decoded.message());
    Image image = decoded.take();
    const int width = image.width;
    const int height = image.height;
    const bool source_is_jpeg = looks_like_jpeg(file_bytes.data(), file_bytes.size());

    // EXIF when present, otherwise SHARP's 30 mm default. Falling back on a
    // malformed Exif segment keeps one bad tag from failing the whole job. The
    // same helper is what the console convert mode uses, so both paths cannot
    // drift.
    const Result<float> focal =
        focal_px_from_photo(file_bytes.data(), file_bytes.size(), width, height);
    if (!focal.ok()) return finish_failed("cannot compute focal length: " + focal.message());
    const float focal_px = focal.value();

    // Both image outputs are made before the RGB buffer moves into the backend.
    const Result<Image> thumbnail = make_thumbnail(image, config_.thumbnail_max_edge);
    if (!thumbnail.ok()) return finish_failed("cannot build thumbnail: " + thumbnail.message());
    const Result<std::vector<uint8_t>> thumbnail_jpeg =
        encode_jpeg(thumbnail.value(), config_.jpeg_quality);
    if (!thumbnail_jpeg.ok()) return finish_failed("cannot encode thumbnail: " + thumbnail_jpeg.message());

    std::vector<uint8_t> original_jpeg;
    if (source_is_jpeg) {
        original_jpeg = std::move(file_bytes);
    } else {
        Result<std::vector<uint8_t>> encoded = encode_jpeg(image, config_.jpeg_quality);
        if (!encoded.ok()) return finish_failed("cannot encode original: " + encoded.message());
        original_jpeg = encoded.take();
    }

    if (cancel_requested()) return finish_cancelled();

    ReconstructionInput input;
    input.rgb = std::move(image);
    input.focal_px = focal_px;

    reporter.report(0.03f, "inferring");
    Progress backend_progress([&reporter](float fraction, const std::string& stage) {
        reporter.report(0.05f + 0.9f * fraction, stage.empty() ? "inferring" : stage);
    });
    Result<SplatScene> scene = reconstructor->run(input, backend_progress);
    if (!scene.ok()) return finish_failed("reconstruction failed: " + scene.message());
    if (!scene.value().consistent()) {
        return finish_failed("reconstruction returned an inconsistent scene");
    }

    if (cancel_requested()) return finish_cancelled();

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const Path folder = unique_scene_folder(album_root(config_.output_root, job.album),
                                            scene_folder_base(job.photo));

    SceneMeta meta;
    meta.backend = reconstructor->info().name.empty() ? job.backend : reconstructor->info().name;
    meta.intrinsics.width = static_cast<uint32_t>(width);
    meta.intrinsics.height = static_cast<uint32_t>(height);
    meta.intrinsics.fx = focal_px;
    meta.intrinsics.fy = focal_px;
    meta.intrinsics.cx = static_cast<float>(width) / 2.0f;
    meta.intrinsics.cy = static_cast<float>(height) / 2.0f;
    meta.seconds_taken = seconds;
    meta.source = source_label(job.photo);

    reporter.report(0.96f, "writing scene");
    const Status written =
        write_scene_folder(folder, scene.value(), meta, original_jpeg, thumbnail_jpeg.value());
    if (!written.ok()) return finish_failed("cannot write scene folder: " + written.message());

    Status marked = Status::success();
    bool was_cancelled = false;
    {
        QueueAccess access(queue_mutex_);
        marked = queue_.mark_done(job.id, folder, seconds);
        if (!marked.ok()) {
            const Job* current = queue_.find(job.id);
            was_cancelled = current != nullptr && current->state != JobState::Running;
        }
    }
    if (!marked.ok()) {
        if (!was_cancelled) return Outcome::failure(marked.message());
        // Cancelled while the scene was being written; keep that state.
        JobOutcome outcome;
        {
            QueueAccess access(queue_mutex_);
            outcome = outcome_from_queue(queue_, job.id);
        }
        return Outcome::success(std::optional<JobOutcome>(std::move(outcome)));
    }

    reporter.report(1.0f, "done");
    JobOutcome outcome;
    {
        QueueAccess access(queue_mutex_);
        outcome = outcome_from_queue(queue_, job.id);
    }
    return Outcome::success(std::optional<JobOutcome>(std::move(outcome)));
}

}  // namespace splat
