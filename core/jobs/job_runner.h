#pragma once

#include <chrono>
#include <functional>
#include <map>
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
    // Where a scene for a network destination is built before it is uploaded
    // (<staging_root>/<job id>). Empty: a remote output_root fails the job with
    // a message instead of writing somewhere unintended. Last so the existing
    // positional initialisers (root, model, edge, quality) keep their meaning.
    Path staging_root;
    // Waiting uploads (0.6.0). A finished scene whose network folder does not
    // answer is kept in its staging folder and the job waits; the upload is
    // retried after retry_initial_seconds, then after twice as long each time up
    // to retry_max_seconds. A job found Waiting at start-up is retried at once.
    double retry_initial_seconds = 5.0;
    double retry_max_seconds = 120.0;
    // The display name of a configured location ("smb0" -> "NAS") for the row text
    // "Waiting for NAS"; empty for an id that is not configured. Empty function:
    // the id is used as the name, and a location with no mounted adapter counts
    // as removed (a failure) rather than as one that is only off.
    std::function<std::string(const std::string& location_id)> location_name;
};

// The wait before upload attempt number `attempt` (1 = the first retry):
// initial, 2x, 4x, ... capped at `max_seconds`.
double upload_retry_delay_seconds(int attempt, double initial_seconds, double max_seconds);

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

// C9. Runs the next queued job. A `convert` job decodes the photo, reads the
// EXIF focal length (SHARP's 30 mm default when there is none), runs the
// backend, then writes a C5 scene folder in the job's album. An `import` job
// takes a different branch before any model is loaded: it reads the splat
// file, thins it to kMaxSceneSplats and writes an imported scene folder
// (core/scene/imported_scene.h). Failures are recorded on the job with a
// message instead of being thrown.
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
// A scene for a network folder is built in a staging folder and uploaded. When
// the folder does not answer at that point (the share is off, the cable is out)
// the job becomes Waiting instead of Failed: the finished scene stays in
// <staging_root>/<job id>, run_next retries the upload on a growing delay and
// saves the scene when the share is back, and cancelling the job removes the
// staging copy. A share that answers and then refuses the write is a real failure
// and keeps the old "upload failed" row.
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
    // Asked before a scene turn starts whether the viewer has that folder open.
    // True refuses the job: rewriting under the loaded scene would leave the
    // renderer and the file disagreeing. Set by the UI adapter; a runner driven
    // straight from a CLI leaves it empty.
    void set_scene_open(std::function<bool(const Path& folder)> open);

    // Runs at most one job. Returns nullopt when nothing is queued or a job is
    // already running. progress receives 0..1 with a short stage label.
    Result<std::optional<JobOutcome>> run_next(const ProgressCallback& progress = {});

    // Seconds until the earliest Waiting job is due for another upload attempt: 0
    // when one is due now, nullopt when nothing waits. The JobService worker
    // sleeps that long when it has nothing else to do.
    std::optional<double> next_retry_seconds() const;
    // Removes a job's staging folder (best effort).
    void discard_staging(const std::string& id) const;
    // The cancel of a Waiting job, from any thread: removes its staging folder at
    // once and has the runner thread remove the hidden temporary folder an upload
    // attempt that dropped may have left on the share (at its next run_next, so
    // the caller never waits on a share that is off).
    void discard_waiting(const std::string& id);

private:
    // The id of the Waiting job to retry now, "" when none is due. Caller holds
    // the queue lock.
    std::string due_waiting_job();
    // True when `output_root`'s location is configured and does not answer: the
    // case a Waiting job exists for. A location that was removed, or a share that
    // answers and then refuses the write, is a real failure instead.
    bool destination_offline(const Path& output_root) const;
    // One upload attempt for a Waiting job: Done when the share took the scene,
    // still Waiting (on a longer delay) when it is still off, Failed when it
    // answers and refuses.
    Result<std::optional<JobOutcome>> retry_upload(const Job& job);
    // <staging_root>/<id>, or an empty path when there is no staging root or the
    // id is not a plain folder name.
    Path staging_folder(const std::string& id) const;

    JobQueue& queue_;
    GpuPhaseGuard& guard_;
    Factory factory_;
    JobRunnerConfig config_;
    std::function<bool(const std::string&)> cancel_request_;
    std::function<bool(const Path&)> scene_open_;
    std::mutex* queue_mutex_ = nullptr;
    // When each Waiting job may try again. Touched only by the runner thread; a
    // job with no entry (found Waiting at start-up) is due at once.
    struct Retry {
        int attempts = 0;
        std::chrono::steady_clock::time_point due;
    };
    std::map<std::string, Retry> retries_;
    // Waiting jobs cancelled by the UI whose share-side temporary folder still
    // has to go (discard_waiting); the runner thread works it off.
    std::mutex abandoned_mutex_;
    std::vector<std::string> abandoned_;
    void remove_abandoned_uploads();
};

}  // namespace splat
