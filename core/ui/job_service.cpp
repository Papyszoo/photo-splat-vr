// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/job_service.h"

#include <utility>

#include "gallery/visibility.h"
#include "vfs/io.h"

namespace splat {
namespace {

// Shown on a running row after a cancel request: the backend has no cancel
// point inside run(), so the job stops at the next stage boundary.
constexpr const char* kCancelPending = "cancels after this photo";

// The visible-folders key of a job's source: the one the picker sent, else the
// path's own mapping (an old queue file, a caller that has no policy context).
// It is written into meta.json for information only; nothing hides a job by it.
std::string job_source_key(const Visibility& visibility, const Path& photo,
                           const std::string& stored) {
    if (!stored.empty()) return stored;
    if (!visibility.has_context()) return std::string();
    return visibility.key_for_source(photo).text();
}

}  // namespace

bool destination_visible(const std::string& album, const Path& output_root) {
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    if (!visibility->enabled) return true;
    if (!output_root.empty() && vfs::is_remote(output_root)) {
        return visibility->allows_location(vfs::location_id(output_root));
    }
    return visibility->allows_album(album);
}

bool job_visible(const JobView& job) {
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    if (!visibility->enabled) return true;
    // The destination half only (stage U1): where the result is written.
    return destination_visible(job.album, job.output_root);
}

JobService::JobService(JobQueue& queue, JobRunner& runner, std::function<void()> on_changed)
    : queue_(queue), runner_(runner), on_changed_(std::move(on_changed)) {
    runner_.set_queue_mutex(&queue_mutex_);
    runner_.set_cancel_request([this](const std::string& id) { return cancel_requested(id); });
    runner_.set_scene_open([this](const Path& folder) {
        std::lock_guard<std::mutex> lock(open_scene_mutex_);
        return !open_scene_.empty() && !folder.empty() &&
               open_scene_.lexically_normal() == folder.lexically_normal();
    });
    worker_ = std::thread(&JobService::worker_main, this);
}

JobService::~JobService() {
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true;
    }
    wake_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // The hooks capture this; nothing may call into a destroyed service.
    runner_.set_cancel_request({});
    runner_.set_scene_open({});
    runner_.set_queue_mutex(nullptr);
}

void JobService::set_open_scene(const Path& folder) {
    std::lock_guard<std::mutex> lock(open_scene_mutex_);
    open_scene_ = folder;
}

void JobService::worker_main() {
    publish();
    while (!stop_.load()) {
        const Result<std::optional<JobOutcome>> outcome = runner_.run_next(
            [this](float fraction, const std::string& stage) { set_progress(fraction, stage); });

        // The job is over (or nothing ran): the next one starts from zero and
        // any cancel request belonged to the finished job.
        {
            std::lock_guard<std::mutex> lock(views_mutex_);
            progress_fraction_ = 0.0f;
            progress_stage_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            cancel_id_.clear();
        }
        publish();
        if (stop_.load()) break;

        // Nothing queued (or the runner refused the job): sleep until enqueue or
        // cancel wakes this thread instead of spinning.
        if (!outcome.ok() || !outcome.value()) wait_for_work();
    }
}

void JobService::wait_for_work() {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_cv_.wait(lock, [this] { return stop_.load() || wake_; });
    wake_ = false;
}

void JobService::set_progress(float fraction, const std::string& stage) {
    {
        std::lock_guard<std::mutex> lock(views_mutex_);
        progress_fraction_ = fraction;
        progress_stage_ = stage;
    }
    publish();
}

void JobService::publish() {
    float fraction = 0.0f;
    std::string stage;
    {
        std::lock_guard<std::mutex> lock(views_mutex_);
        fraction = progress_fraction_;
        stage = progress_stage_;
    }

    std::vector<JobView> views;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const std::vector<Job>& jobs = queue_.jobs();
        views.reserve(jobs.size());
        for (const Job& job : jobs) {
            JobView view;
            view.id = job.id;
            view.name = job.photo.filename().string();
            view.photo = job.photo;
            if (view.name.empty()) view.name = job.photo.string();
            // An import has no model; the row and the console log say "import"
            // instead of leaving the field empty. A turn has neither and says
            // "rotate".
            view.backend = job.kind == JobKind::import
                               ? std::string("import")
                               : (job.kind == JobKind::rotate ? std::string("rotate")
                                                              : job.backend);
            view.album = job.album;
            view.kind = job_kind_name(job.kind);
            view.state = job_state_name(job.state);
            view.message = job.message;
            view.source_folder = job.source_folder;
            view.output_root = job.output_root;
            view.scene_folder = job.scene_folder;
            if (job.state == JobState::Running) {
                view.fraction = fraction;
                view.stage = stage.empty() ? std::string("starting") : stage;
                if (job.id == cancel_id_) {
                    view.message = job.kind == JobKind::rotate
                                       ? std::string("cancels after this scene")
                                       : (job.kind == JobKind::import
                                              ? std::string("cancels after this file")
                                              : std::string(kCancelPending));
                }
            } else {
                view.stage = view.state;
            }
            views.push_back(std::move(view));
        }
    }

    {
        std::lock_guard<std::mutex> lock(views_mutex_);
        views_ = std::move(views);
    }
    notify_changed();
}

void JobService::notify_changed() {
    if (on_changed_) on_changed_();
}

bool JobService::cancel_requested(const std::string& id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !id.empty() && cancel_id_ == id;
}

Status JobService::enqueue(const std::vector<PhotoPick>& photos, const std::string& backend,
                           const std::string& album, const Path& output_root) {
    if (photos.empty()) return Status::error("no photos to convert");
    if (backend.empty()) return Status::error("backend name is empty");
    // The destination is one tick for the whole batch: a hidden album or
    // location refuses the queue before anything is added. The photo's source
    // folder does not (stage U1: photo folders are not hidden).
    if (!destination_visible(album, output_root)) return Status::error(kNotVisibleText);

    const std::shared_ptr<const Visibility> visibility = current_visibility();
    std::size_t queued = 0;
    std::string first_error;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const PhotoPick& photo : photos) {
            // The folder each photo came from, as the picker saw it, written
            // into the scene's meta.json; it is not a filter.
            const std::string key = job_source_key(*visibility, photo.path, photo.source_folder);
            const Result<std::string> added = queue_.enqueue(photo.path, backend, album,
                                                             photo.rotate_quarters, output_root,
                                                             key);
            if (added.ok()) {
                ++queued;
                continue;
            }
            if (first_error.empty()) first_error = added.message();
        }
    }

    if (queued == 0) {
        return Status::error(first_error.empty() ? "nothing was queued" : first_error);
    }
    publish();
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true;
    }
    wake_cv_.notify_all();
    return Status::success();
}

Status JobService::enqueue_import(const std::vector<Path>& splats, const std::string& album,
                                  std::size_t* queued_out, const Path& output_root,
                                  const std::string& source_folder) {
    if (splats.empty()) return Status::error("no splat files to import");
    if (!destination_visible(album, output_root)) return Status::error(kNotVisibleText);

    const std::shared_ptr<const Visibility> visibility = current_visibility();
    std::size_t queued = 0;
    std::string first_error;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const Path& splat : splats) {
            const std::string key = job_source_key(*visibility, splat, source_folder);
            const Result<std::string> added =
                queue_.enqueue_import(splat, album, output_root, key);
            if (added.ok()) {
                ++queued;
                continue;
            }
            if (first_error.empty()) first_error = added.message();
        }
    }

    if (queued_out != nullptr) *queued_out = queued;
    if (queued == 0) {
        return Status::error(first_error.empty() ? "nothing was queued" : first_error);
    }
    publish();
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true;
    }
    wake_cv_.notify_all();
    return Status::success();
}

Status JobService::enqueue_rotate(const Path& scene_folder, int quarters,
                                  const std::string& album) {
    if (scene_folder.empty()) return Status::error("no scene folder to rotate");
    Status status = Status::success();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const Result<std::string> added = queue_.enqueue_rotate(scene_folder, quarters, album);
        if (!added.ok()) return Status::error(added.message());
    }
    publish();
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_ = true;
    }
    wake_cv_.notify_all();
    return status;
}

std::vector<JobView> JobService::snapshot() const {
    std::lock_guard<std::mutex> lock(views_mutex_);
    // A filtered copy: publish()/views_ keep every row, so unlocking (or
    // turning the passcode off) brings the hidden ones back without losing
    // their progress.
    if (!current_visibility()->enabled) return views_;
    std::vector<JobView> visible;
    visible.reserve(views_.size());
    for (const JobView& view : views_) {
        if (job_visible(view)) visible.push_back(view);
    }
    return visible;
}

Status JobService::cancel(const std::string& id) {
    Status status = Status::success();
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const Job* job = queue_.find(id);
        if (job == nullptr) return Status::error("no such job: " + id);
        if (job->state == JobState::Running) {
            cancel_id_ = id;
            running = true;
        } else {
            status = queue_.cancel(id);
        }
    }

    publish();
    if (running) {
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            wake_ = true;
        }
        wake_cv_.notify_all();
    }
    return status;
}

bool JobService::busy() const {
    std::lock_guard<std::mutex> lock(views_mutex_);
    for (const JobView& view : views_) {
        if (view.state == "running") return true;
    }
    return false;
}

}  // namespace splat
