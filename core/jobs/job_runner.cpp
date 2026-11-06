#include "jobs/job_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/progress.h"
#include "gallery/visibility.h"
#include "jobs/image_io.h"
#include "scene/imported_scene.h"
#include "scene/scene_folder.h"
#include "scene/scene_rotate.h"
#include "scene/splat_scene.h"
#include "splat_import/splat_import.h"
#include "splat_import/thinning.h"
#include "vfs/io.h"

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
    vfs::FileStat status;
    return vfs::stat(path, &status).ok() && status.exists;
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

// The visible-folders key the scene's meta.json records. The picker put it on
// the job, so a job resumed at boot writes what the owner saw when it was
// queued. An old queued job has none: the process policy then derives one from
// the photo path, and a build with no policy context (tests, tools) leaves the
// field empty exactly as before stage H.
std::string scene_source_folder(const Job& job) {
    if (!job.source_folder.empty()) return job.source_folder;
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    if (!visibility->has_context()) return std::string();
    return visibility->key_for_source(job.photo).text();
}

// Copies a finished scene from the local staging folder to its destination on
// a network location and checks that every file arrived whole. The staging copy
// is removed only when the destination really holds the scene: an interrupted
// upload leaves it for the operator (the failure message names it).
Status upload_scene(const Path& staging, const Path& destination) {
    std::vector<vfs::DirEntry> staged;
    const Status listed = vfs::list(staging, &staged);
    if (!listed.ok()) return Status::error(listed.message());
    // "Every file arrived" is vacuous for an empty folder, which would publish
    // an empty scene on the share and report Done. Nothing this build writes is
    // empty, so an empty staging folder is a writer bug, not a scene.
    bool any_file = false;
    for (const vfs::DirEntry& entry : staged) {
        if (!entry.directory && !entry.symlink) any_file = true;
    }
    if (!any_file) {
        return Status::error("the staging folder " + staging.string() + " is empty");
    }

    const Status copied = vfs::copy_tree(staging, destination);
    if (!copied.ok()) return Status::error(copied.message());

    for (const vfs::DirEntry& entry : staged) {
        if (entry.directory || entry.symlink) continue;
        vfs::FileStat source;
        vfs::FileStat target;
        if (!vfs::stat(staging / entry.name, &source).ok() || !source.regular) {
            return Status::error("cannot verify " + entry.name);
        }
        if (!vfs::stat(destination / entry.name, &target).ok() || !target.regular) {
            return Status::error("uploaded " + entry.name + " is missing");
        }
        if (source.size != target.size) {
            return Status::error("uploaded " + entry.name + " is " +
                                 std::to_string(target.size) + " bytes, expected " +
                                 std::to_string(source.size));
        }
    }
    return vfs::remove_tree(staging);
}

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

void JobRunner::set_scene_open(std::function<bool(const Path& folder)> open) {
    scene_open_ = std::move(open);
}

Result<std::optional<JobOutcome>> JobRunner::run_next(const ProgressCallback& progress) {
    using Outcome = Result<std::optional<JobOutcome>>;

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

    auto finish_done = [&](const Path& folder, double seconds, std::string message) -> Outcome {
        Status marked = Status::success();
        bool was_cancelled = false;
        {
            QueueAccess access(queue_mutex_);
            marked = queue_.mark_done(job.id, folder, seconds, std::move(message));
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
    };

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

    // A scene turn is a file job: no model, no GPU guard, and no output_root -
    // the folder it rewrites is the one it was handed. It is checked before the
    // conversion destination because a turn has none.
    if (job.kind == JobKind::rotate) {
        // The scene may have been opened after the turn was queued (behind a
        // running conversion): the renderer holds the version it read, so the
        // rewrite must not happen under it.
        if (scene_open_ && scene_open_(job.photo)) {
            return finish_failed("close the scene before rotating it");
        }
        reporter.report(0.05f, "rotating");
        if (cancel_requested()) return finish_cancelled();
        const Status rotated = rotate_scene_folder(job.photo, job.rotate_quarters);
        if (!rotated.ok()) return finish_failed(rotated.message());
        if (cancel_requested()) return finish_cancelled();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        reporter.report(0.98f, "writing scene");
        return finish_done(job.photo, seconds, "rotated");
    }

    // The destination root: the job's own (a network library the picker was
    // showing) or the runner's configured local root.
    const Path output_root = job.output_root.empty() ? config_.output_root : job.output_root;
    if (output_root.empty()) return finish_failed("scene output root is not configured");
    const bool upload = vfs::is_remote(output_root);
    if (upload && config_.staging_root.empty()) {
        return finish_failed("cannot upload to " + output_root.string() +
                             ": no staging folder is configured");
    }

    // A remote destination is built in <staging_root>/<job id> and uploaded
    // when the scene folder is complete; a local one is written in place.
    const auto write_target = [&](const Path& destination, Path* folder) -> Status {
        if (!upload) {
            *folder = destination;
            return Status::success();
        }
        *folder = config_.staging_root / job.id;
        const Status cleared = vfs::remove_tree(*folder);  // a retried job starts clean
        if (!cleared.ok() && path_exists(*folder)) return cleared;
        return Status::success();
    };
    const auto publish = [&](const Path& folder, const Path& destination) -> Status {
        if (!upload) return Status::success();
        const Status uploaded = upload_scene(folder, destination);
        if (uploaded.ok()) return Status::success();
        return Status::error("upload failed: " + uploaded.message() + "; kept at " +
                             folder.string());
    };

    // An import is decided before any model exists: no backend is loaded, and
    // the GPU phase guard is not taken (a splat import allocates no GPU
    // memory, so a viewer may keep drawing while it runs).
    if (job.kind == JobKind::import) {
        ImportLimits limits;
        vfs::FileStat status;
        if (!vfs::stat(job.photo, &status).ok() || !status.regular) {
            return finish_failed("cannot read " + job.photo.string());
        }
        const uint64_t declared_bytes = status.size;
        if (declared_bytes > limits.max_bytes) {
            return finish_failed("splat file is " + std::to_string(declared_bytes) +
                                 " bytes, larger than the 536870912-byte (512 MiB) import limit");
        }

        reporter.report(0.02f, "reading");
        Result<std::vector<uint8_t>> read = read_file_bytes(job.photo, limits.max_bytes);
        if (!read.ok()) return finish_failed("cannot read splat file: " + read.message());
        if (cancel_requested()) return finish_cancelled();

        reporter.report(0.1f, "importing");
        ImportReport report;
        Result<SplatScene> imported = import_splat(job.photo, read.value().data(),
                                                   read.value().size(), limits, &report);
        // The file bytes are 28 B per splat on top of the scene; release them
        // before the thinning copy doubles the scene briefly.
        read = Result<std::vector<uint8_t>>::success(std::vector<uint8_t>());
        if (!imported.ok()) return finish_failed("cannot import splat: " + imported.message());

        SplatScene scene = imported.take();
        thin_scene(scene, limits.max_splats, &report);
        if (!scene.consistent()) return finish_failed("imported splat scene is inconsistent");
        if (cancel_requested()) return finish_cancelled();

        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const Path destination = unique_scene_folder(album_root(output_root, job.album),
                                                     scene_folder_base(job.photo));
        Path folder;
        const Status staged = write_target(destination, &folder);
        if (!staged.ok()) return finish_failed(staged.message());

        SceneMeta meta;
        meta.backend = "import";
        meta.origin_format = splat_format_name(report.format);
        meta.origin_count = report.source_count;
        meta.seconds_taken = seconds;
        meta.source = source_label(job.photo);
        meta.source_folder = scene_source_folder(job);

        reporter.report(0.9f, "writing scene");
        const int edge = std::max(64, config_.thumbnail_max_edge);
        const std::vector<uint8_t> thumb = make_import_thumb(scene, edge, edge * 9 / 16);
        if (thumb.empty()) return finish_failed("cannot build the imported scene thumbnail");
        const Status written = write_imported_scene(folder, scene, meta, thumb);
        if (!written.ok()) return finish_failed("cannot write scene folder: " + written.message());
        const Status published = publish(folder, destination);
        if (!published.ok()) return finish_failed(published.message());

        const std::string message =
            report.thinned
                ? "thinned " + count_label(report.source_count) + " to " +
                      count_label(report.kept_count) + " splats"
                : "imported " + count_label(report.kept_count) + " splats";
        return finish_done(upload ? destination : folder, seconds, message);
    }

    ScopedInference inference(guard_);

    // Closing down while waiting for a viewer is not a failed conversion, but
    // the job cannot stay Running either: record it and let the caller decide.
    if (!inference.acquired()) {
        return finish_failed(
            "interrupted: the GPU phase guard is shutting down, so this job did not run");
    }

    if (!factory_) {
        return finish_failed("no reconstruction backend factory configured");
    }

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

    Result<std::vector<uint8_t>> read =
        read_file_bytes(job.photo, image::DecodeOptions{}.max_bytes);
    if (!read.ok()) return finish_failed("cannot read photo: " + read.message());
    std::vector<uint8_t> file_bytes = read.take();

    // The job's quarter turns are applied by the decoder, after the EXIF
    // orientation, so the thumbnail, the reconstruction input and the meta
    // intrinsics all describe the same image.
    image::DecodeOptions decode_options;
    decode_options.rotate_quarters = job.rotate_quarters;
    // .nef/.arw/.cr3/... are TIFF or unknown containers; the extension is the
    // only thing that says they are RAW.
    decode_options.hint = image_format_from_extension(job.photo);
    image::DecodeInfo decode_info;
    Result<Image> decoded =
        image::decode_photo(file_bytes.data(), file_bytes.size(), decode_options, &decode_info);
    if (!decoded.ok()) return finish_failed("cannot decode photo: " + decoded.message());
    Image image = decoded.take();
    // Verbatim only when the pixels really are the file's own: an oriented or
    // manually rotated JPEG would otherwise carry the wrong dimensions.
    const bool source_is_jpeg = looks_like_jpeg(file_bytes.data(), file_bytes.size()) &&
                                decode_info.exif_orientation == 1 &&
                                ((job.rotate_quarters % 4) + 4) % 4 == 0;

    // EXIF when present, otherwise SHARP's 30 mm default, mapped into the
    // decoded image with the same transform the pixels went through. A
    // malformed Exif segment falls back to the default.
    const Result<Intrinsics> intrinsics = photo_intrinsics(file_bytes.data(), file_bytes.size(),
                                                           decode_info);
    if (!intrinsics.ok()) {
        return finish_failed("cannot compute focal length: " + intrinsics.message());
    }
    const float focal_px = intrinsics.value().fx;

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
    const Path destination = unique_scene_folder(album_root(output_root, job.album),
                                                 scene_folder_base(job.photo));
    Path folder;
    const Status staged = write_target(destination, &folder);
    if (!staged.ok()) return finish_failed(staged.message());

    SceneMeta meta;
    meta.backend = reconstructor->info().name.empty() ? job.backend : reconstructor->info().name;
    meta.intrinsics = intrinsics.value();
    meta.seconds_taken = seconds;
    meta.source = source_label(job.photo);
    meta.source_folder = scene_source_folder(job);

    reporter.report(0.96f, "writing scene");
    const Status written =
        write_scene_folder(folder, scene.value(), meta, original_jpeg, thumbnail_jpeg.value());
    if (!written.ok()) return finish_failed("cannot write scene folder: " + written.message());
    const Status published = publish(folder, destination);
    if (!published.ok()) return finish_failed(published.message());

    // Which decode path ran is the release claim about RAW thumbnails ("the
    // camera's preview, not a dark frame"), and only the decoder knows it. It
    // goes on the job's own done line, where the console check already greps:
    // "viewer: jobs: file.dng done: raw preview (SONY)" (console pass 3, P3).
    return finish_done(upload ? destination : folder, seconds, decode_info.note);
}

}  // namespace splat
