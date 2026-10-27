#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "common/path.h"
#include "common/status.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "recon/reconstructor.h"

namespace splat {

struct JobRunnerConfig {
    // Scenes go to output_root/<album>/<scene folder name>; the album is "" for
    // the loose-scene album at the root.
    Path output_root;
    Path model_path;                  // GGUF handed to IReconstructor::load
    int thumbnail_max_edge = 512;     // thumb.jpg longest edge
    int jpeg_quality = 90;            // original.jpg (re-encode) and thumb.jpg
};

// The folder name a job for `photo` gets: the photo stem with every character
// outside [A-Za-z0-9._-] replaced by '_', at most 48 characters, and never a
// hidden name (a leading '.' becomes '_'). An empty result becomes "photo". A
// name that already exists under the album gets -2, -3, ... appended by the
// runner, so the UI can look the scene up by base name (core/ui/screens/queue).
std::string scene_folder_base(const Path& photo);

// One finished job, as the queue recorded it. The UI can show the folder and
// the message without a second lookup.
struct JobOutcome {
    std::string job_id;
    JobState state = JobState::Failed;
    std::string message;
    Path scene_folder;
    double seconds_taken = 0.0;
};

// C9. Runs the next queued job: take it, decode the photo, read the EXIF focal
// length (SHARP's 30 mm default when there is none), run the backend, then
// write a C5 scene folder in the job's album. Failures are recorded on the job
// with a message instead of being thrown.
//
// A crash mid-job is safe from both sides: the queue file says "running", which
// open() re-queues, and the scene folder is only valid once meta.json is
// written (write_scene_folder writes it last), so a half-written folder is
// rewritten by the next attempt rather than read.
//
// The GPU phase guard is held for the whole conversion. The backend is
// unloaded and reported unloaded before the guard admits viewers, so the model
// is never resident while the renderer allocates. If the guard is shut down
// while this thread waits for a viewer, run_next records the job as
// interrupted instead of leaving it Running.
//
// One thread runs the runner, and that thread owns the queue. A UI adapter
// (core/ui/job_service.h) that also enqueues and cancels sets a queue mutex
// with set_queue_mutex() and a cancel request with set_cancel_request(); both
// are configured before the first run_next().
class JobRunner {
public:
    // Returns the backend for a job's name, or nullptr when none is registered.
    using Factory = std::function<std::unique_ptr<IReconstructor>(const std::string& backend)>;

    JobRunner(JobQueue& queue, GpuPhaseGuard& guard, Factory factory, JobRunnerConfig config);

    // Asked at every stage boundary (after loading, after decoding, after
    // inference, before the scene is written) with the running job id. True
    // asks the runner to cancel that job: it is recorded as Cancelled and no
    // scene folder is written. Called on the runner thread without any queue
    // lock held, so the callback may take its own locks. A job that is mid
    // inference only observes the request when the reconstructor returns.
    void set_cancel_request(std::function<bool(const std::string& id)> request);
    // The lock the caller takes around its own queue access. The runner takes
    // it around every queue call and never holds it during inference, so an
    // enqueue on another thread does not wait for a conversion.
    void set_queue_mutex(std::mutex* mutex);

    // Runs at most one job. Returns nullopt when nothing is queued or a job is
    // already running. progress receives 0..1 with a short stage label.
    Result<std::optional<JobOutcome>> run_next(const ProgressCallback& progress = {});

private:
    JobQueue& queue_;
    GpuPhaseGuard& guard_;
    Factory factory_;
    JobRunnerConfig config_;
    std::function<bool(const std::string&)> cancel_request_;
    std::mutex* queue_mutex_ = nullptr;
};

}  // namespace splat
