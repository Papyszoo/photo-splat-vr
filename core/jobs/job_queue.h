#pragma once

#include <cstddef>
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

// What a job does. `convert` runs the reconstruction backend on a photo;
// `import` reads an already-converted splat file and needs no model at all;
// `rotate` turns a scene folder on disk about its viewing axis and needs no
// model either. Queue files written before L2 have no "kind" key and load as
// convert.
enum class JobKind { convert, import, rotate };

const char* job_kind_name(JobKind kind);
std::optional<JobKind> job_kind_from_name(const std::string& name);

struct Job {
    std::string id;
    Path photo;
    JobKind kind = JobKind::convert;
    std::string backend;  // "" for an import: there is no model
    // Target album under the scene root; "" is the loose-scene album. Older
    // queue files have no such key and load as "".
    std::string album;
    // Clockwise quarter turns to apply before reconstruction (the photo picker's
    // Square). 0 is "as the camera stored it". Older queue files have no key
    // and load as 0.
    int rotate_quarters = 0;
    // The library root the scene goes into, when it is not the runner's local
    // scene root: a location path such as `smb0:/scenes`. Empty (the old
    // format) means the runner's configured root.
    Path output_root;
    // The visible-folders key of the folder the photo/file came from
    // ("source:inbox", "folder:/data/Photos", ...; core/gallery/visibility.h).
    // Carried from the screen through the queue file to the scene's meta.json,
    // so a job resumed at boot still writes the provenance the picker saw
    // rather than one derived from a policy the owner may have changed since.
    // Empty in queue files written before stage H.
    std::string source_folder;
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
    // The largest queue file this build will read. A conversion row is a few
    // hundred bytes, so this leaves room for a full kMaxJobs queue (20000 rows
    // at ~400 bytes is 7.6 MiB) and is still small enough to refuse a foreign
    // multi-hundred-megabyte file without parsing it. save() enforces the same
    // limit, so a file this build wrote is never refused by open() (the 0.5.0
    // dev title crashed on a state file whose row count came out as 1.1e19;
    // REPORT-CR.md).
    static constexpr std::size_t kMaxFileBytes = 16u << 20;
    // Rows one file may hold. The same rule for the count the log prints and the
    // worker walks: a count that cannot be real is refused before any row is
    // built, never handed to a vector or a reserve(). add_job() enforces it, so
    // this build never writes a queue its reader would refuse.
    static constexpr std::size_t kMaxJobs = 20000;
    // One string field (a path, a message) longer than this is not a path or a
    // message this title wrote; a truncated file tends to produce one.
    static constexpr std::size_t kMaxFieldChars = 4096;
    // The largest sequence a queue file may carry, for a row's sequence and for
    // next_sequence. The id is "job-" plus the sequence; a live queue stays far
    // below this, so anything above it came from a corrupt or foreign file. A
    // row's sequence must stay below it, so duplicate-id-free ids never run out
    // (next_sequence_ may reach it; enqueueing from there is refused).
    static constexpr uint64_t kMaxSequence = 1000000000ull;

    // Reads the file when it exists, otherwise starts an empty queue. Malformed
    // JSON, a truncated file or an implausible count/size is an error instead of
    // a silent reset, so an operator can inspect it; the message says which.
    static Result<JobQueue> open(const Path& state_file);

    // open() for a boot path, which must start whatever the last session left:
    // a file that cannot be read, parsed or believed is moved aside to
    // "<file>.rejected" (best effort) and the queue starts empty. `notice`, when
    // given, receives the one-line reason for the log; it is empty on success.
    // Never fails.
    static JobQueue open_or_empty(const Path& state_file, std::string* notice);

    // An empty queue that writes to `state_file`. For a host that refused a file
    // (open_or_empty) or a test that needs a queue with no file behind it.
    static JobQueue empty(const Path& state_file);

    const Path& state_file() const { return state_file_; }
    const std::vector<Job>& jobs() const { return jobs_; }
    const Job* find(const std::string& id) const;

    // Adds a queued job. photo is normalized and the call fails when the same
    // file is already queued or running, so a watcher cannot enqueue twice.
    // album is the scene album the finished scene goes into ("" = the
    // loose-scene album at the root). `output_root`, when set, is the library
    // root for this job only (a network location): the runner stages the scene
    // locally and uploads it there.
    Result<std::string> enqueue(const Path& photo, std::string backend,
                               std::string album = std::string(), int rotate_quarters = 0,
                               const Path& output_root = Path(),
                               std::string source_folder = std::string());

    // Queues the import of one already-converted splat file into `album`. The
    // job has no backend and no rotation; it fails at run time rather than here
    // when the file cannot be read, so the row carries the reader's message.
    Result<std::string> enqueue_import(const Path& splat_file,
                                       std::string album = std::string(),
                                       const Path& output_root = Path(),
                                       std::string source_folder = std::string());

    // Queues a turn of one scene folder by `quarters` clockwise 90-degree steps
    // (3 = one step left). The folder is stored in `photo`, the turn in
    // `rotate_quarters`, and `album` is the shelf it was picked from (empty =
    // the loose-scene album), so the Queue row can name it. The job writes the
    // scene's own files. Duplicate protection is the same as a conversion: a
    // folder already queued or being turned is refused.
    Result<std::string> enqueue_rotate(const Path& scene_folder, int quarters,
                                       std::string album = std::string());

    // Moves the oldest queued job to Running. Returns nullopt when nothing is
    // queued or another job is already running (one conversion at a time).
    Result<std::optional<Job>> take_next();

    // Running -> Done. scene_folder and seconds_taken are stored; a cancelled
    // job is never overwritten. `message` is the row's notice ("imported 1,234
    // splats", "thinned 4,000,000 to 2,000,000 splats") and is empty for a
    // conversion, which has nothing extra to say.
    Status mark_done(const std::string& id, const Path& scene_folder, double seconds_taken,
                     std::string message = std::string());

    // Running -> Failed with the message the UI shows.
    Status mark_failed(const std::string& id, std::string message);

    // Queued or Running -> Cancelled.
    Status cancel(const std::string& id);

    Status save() const;

private:
    Job* mutable_find(const std::string& id);
    static Path normalize(const Path& photo);
    // The shared body of enqueue()/enqueue_import(): the duplicate check, the
    // id, the save and the rollback on a save failure.
    Result<std::string> add_job(const Path& photo, JobKind kind, std::string backend,
                                std::string album, int rotate_quarters,
                                const Path& output_root, std::string source_folder);

    Path state_file_;
    std::vector<Job> jobs_;
    uint64_t next_sequence_ = 1;
};

}  // namespace splat
