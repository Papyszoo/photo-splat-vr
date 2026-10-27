// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/job_service.h"

#include <utility>

namespace splat {
namespace {

// Shown on a running row after a cancel request: the backend has no cancel
// point inside run(), so the job stops at the next stage boundary.
constexpr const char* kCancelPending = "cancels after this photo";

}  // namespace

JobService::JobService(JobQueue& queue, JobRunner& runner, std::function<void()> on_changed)
    : queue_(queue), runner_(runner), on_changed_(std::move(on_changed)) {
    runner_.set_queue_mutex(&queue_mutex_);
    runner_.set_cancel_request([this](const std::string& id) { return cancel_requested(id); });
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
    runner_.set_queue_mutex(nullptr);
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
            view.backend = job.backend;
            view.album = job.album;
            view.state = job_state_name(job.state);
            view.message = job.message;
            if (job.state == JobState::Running) {
                view.fraction = fraction;
                view.stage = stage.empty() ? std::string("starting") : stage;
                if (job.id == cancel_id_) view.message = kCancelPending;
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

Status JobService::enqueue(const std::vector<Path>& photos, const std::string& backend,
                           const std::string& album) {
    if (photos.empty()) return Status::error("no photos to convert");
    if (backend.empty()) return Status::error("backend name is empty");

    std::size_t queued = 0;
    std::string first_error;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const Path& photo : photos) {
            const Result<std::string> added = queue_.enqueue(photo, backend, album);
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

std::vector<JobView> JobService::snapshot() const {
    std::lock_guard<std::mutex> lock(views_mutex_);
    return views_;
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
