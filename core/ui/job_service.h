// IJobService over the real job queue and runner (W12).
//
// One worker thread owns the queue: it loops run_next(progress) and publishes a
// JobView list under a mutex after every stage, so snapshot() never blocks on a
// conversion. The UI thread only enqueues and cancels; both take the queue
// mutex the runner also takes around its queue calls (never during inference),
// so the panel stays responsive while a job runs.
//
// A cancel of the running job is a request, applied by the runner at its next
// stage boundary: mid inference there is no cancel point inside the backend, so
// the job stops after the photo it is working on and the view says so until
// then. The worker is joined in the destructor; a job that was still Running
// stays in the queue file as Running and JobQueue::open() re-queues it on the
// next start (nothing here marks it failed just because the UI closed).
//
// on_changed is called from the worker thread after every published change
// (progress, a finished job, an enqueue/cancel), so a host can invalidate the
// panel from it; it must not block or touch the service.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "ui/services.h"

namespace splat {

class JobService : public IJobService {
public:
    // `queue` and `runner` must outlive the service. The runner must not be
    // used by another thread once the service is constructed (app mode and the
    // CLIs drive their own runner instead of a JobService).
    JobService(JobQueue& queue, JobRunner& runner, std::function<void()> on_changed = {});
    ~JobService() override;

    JobService(const JobService&) = delete;
    JobService& operator=(const JobService&) = delete;

    // One conversion per photo into `album`. Fails when nothing was queued
    // (all photos already active, or an unreadable state file).
    Status enqueue(const std::vector<Path>& photos, const std::string& backend,
                   const std::string& album) override;
    // Oldest first (the queue's enqueue order), done and failed rows kept for
    // the session.
    std::vector<JobView> snapshot() const override;
    // Queued: cancelled at once. Running: cancelled at the next stage boundary
    // (the view keeps "running" and says it cancels after this photo).
    Status cancel(const std::string& id) override;
    bool busy() const override;

private:
    void worker_main();
    // Worker thread only: applies pending state and republishes.
    void publish();
    void notify_changed();
    bool cancel_requested(const std::string& id);
    void set_progress(float fraction, const std::string& stage);
    void wait_for_work();

    JobQueue& queue_;
    JobRunner& runner_;
    std::function<void()> on_changed_;

    // The queue lock the runner shares; see JobRunner::set_queue_mutex.
    mutable std::mutex queue_mutex_;
    // The published views and the progress of the running job.
    mutable std::mutex views_mutex_;
    std::vector<JobView> views_;
    float progress_fraction_ = 0.0f;
    std::string progress_stage_;
    // Id of the running job a cancel was requested for; guarded by queue_mutex_.
    std::string cancel_id_;

    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_ = false;
};

}  // namespace splat
