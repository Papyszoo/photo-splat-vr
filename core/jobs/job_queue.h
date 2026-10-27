#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// C9 job states. Queued and Running are active; the other three are terminal.
enum class JobState { Queued, Running, Done, Failed, Cancelled };

const char* job_state_name(JobState state);
std::optional<JobState> job_state_from_name(const std::string& name);

struct Job {
    std::string id;
    Path photo;
    std::string backend;
    // Target album under the scene root; "" is the loose-scene album. Older
    // queue files have no such key and load as "".
    std::string album;
    JobState state = JobState::Queued;
    std::string message;      // failure or cancellation reason
    Path scene_folder;        // set when the job is done
    double seconds_taken = 0.0;
    uint64_t sequence = 0;    // enqueue order, also the numeric part of the id
};

// C9. One JSON file in the app state directory. Every mutation saves
// immediately through a temp file and a rename, so a crash or a restart keeps
// the queue. A job that was Running when the process died is re-queued on load.
//
// Not thread-safe: the runner loop owns the queue. A UI adapter that also
// enqueues or cancels (core/ui/job_service.h) gives the runner a mutex it takes
// around every queue call; the runner never holds it during inference.
class JobQueue {
public:
    // Reads the file when it exists, otherwise starts an empty queue. Malformed
    // JSON is an error instead of a silent reset, so an operator can inspect it.
    static Result<JobQueue> open(const Path& state_file);

    const Path& state_file() const { return state_file_; }
    const std::vector<Job>& jobs() const { return jobs_; }
    const Job* find(const std::string& id) const;

    // Adds a queued job. photo is normalized and the call fails when the same
    // file is already queued or running, so a watcher cannot enqueue twice.
    // album is the scene album the finished scene goes into ("" = the
    // loose-scene album at the root).
    Result<std::string> enqueue(const Path& photo, std::string backend,
                               std::string album = std::string());

    // Moves the oldest queued job to Running. Returns nullopt when nothing is
    // queued or another job is already running (one conversion at a time).
    Result<std::optional<Job>> take_next();

    // Running -> Done. scene_folder and seconds_taken are stored; a cancelled
    // job is never overwritten.
    Status mark_done(const std::string& id, const Path& scene_folder, double seconds_taken);

    // Running -> Failed with the message the UI shows.
    Status mark_failed(const std::string& id, std::string message);

    // Queued or Running -> Cancelled.
    Status cancel(const std::string& id);

    Status save() const;

private:
    Job* mutable_find(const std::string& id);
    static Path normalize(const Path& photo);

    Path state_file_;
    std::vector<Job> jobs_;
    uint64_t next_sequence_ = 1;
};

}  // namespace splat
