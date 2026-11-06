#include "jobs/job_queue.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

#include "vfs/atomic_write.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr int kQueueFormatVersion = 1;

std::string job_id_for(uint64_t sequence) {
    std::ostringstream out;
    out << "job-" << std::setw(6) << std::setfill('0') << sequence;
    return out.str();
}

}  // namespace

const char* job_state_name(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Done: return "done";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

std::optional<JobState> job_state_from_name(const std::string& name) {
    if (name == "queued") return JobState::Queued;
    if (name == "running") return JobState::Running;
    if (name == "done") return JobState::Done;
    if (name == "failed") return JobState::Failed;
    if (name == "cancelled") return JobState::Cancelled;
    return std::nullopt;
}

const char* job_kind_name(JobKind kind) {
    switch (kind) {
        case JobKind::convert: return "convert";
        case JobKind::import: return "import";
        case JobKind::rotate: return "rotate";
    }
    return "convert";
}

std::optional<JobKind> job_kind_from_name(const std::string& name) {
    if (name == "convert") return JobKind::convert;
    if (name == "import") return JobKind::import;
    if (name == "rotate") return JobKind::rotate;
    return std::nullopt;
}

Result<JobQueue> JobQueue::open(const Path& state_file) {
    JobQueue queue;
    queue.state_file_ = state_file;

    vfs::FileStat inspected;
    if (!vfs::stat(state_file, &inspected).ok()) {
        return Result<JobQueue>::failure("cannot inspect " + state_file.string());
    }
    if (!inspected.exists) return Result<JobQueue>::success(std::move(queue));

    // The size on disk is checked before the bytes are read: a file that is not
    // a queue cannot be allowed to decide how much memory this process asks for.
    if (inspected.size > kMaxFileBytes) {
        return Result<JobQueue>::failure(
            "job queue " + state_file.string() + " is " + std::to_string(inspected.size) +
            " bytes, more than the " + std::to_string(kMaxFileBytes) +
            " byte limit; not a queue file this build wrote");
    }

    const Result<std::vector<uint8_t>> bytes = vfs::read_file(state_file, kMaxFileBytes);
    if (!bytes.ok()) return Result<JobQueue>::failure(bytes.message());
    const std::string text(bytes.value().begin(), bytes.value().end());
    // A write that stopped halfway leaves a prefix of valid JSON that no parser
    // accepts; the parser's message names it, and nothing is kept.
    if (text.size() != inspected.size) {
        return Result<JobQueue>::failure(
            "job queue " + state_file.string() + " read " + std::to_string(text.size()) +
            " of " + std::to_string(inspected.size) + " bytes: the file is truncated");
    }

    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) return Result<JobQueue>::failure("job queue root is not an object");
        if (!root.contains("jobs") || !root.at("jobs").is_array()) {
            return Result<JobQueue>::failure("job queue has no \"jobs\" array in " +
                                             state_file.string());
        }
        // The row count is the first thing read and the first thing validated:
        // every allocation below (the vector, its rows' strings) follows from
        // it, so an impossible count is refused here, not walked later.
        const std::size_t rows = root.at("jobs").size();
        if (rows > kMaxJobs) {
            return Result<JobQueue>::failure(
                "job queue " + state_file.string() + " holds " + std::to_string(rows) +
                " row(s), more than the " + std::to_string(kMaxJobs) +
                " row limit; not a queue file this build wrote");
        }

        // One string field, bounded: a path or a message this title wrote is far
        // under the limit, and a truncated or foreign file produces longer ones.
        const auto bounded = [&state_file](const nlohmann::json& entry, const char* key,
                                           std::string fallback) -> Result<std::string> {
            if (!entry.contains(key) || entry.at(key).is_null()) {
                return Result<std::string>::success(std::move(fallback));
            }
            if (!entry.at(key).is_string()) {
                return Result<std::string>::failure(std::string("job queue field \"") + key +
                                                    "\" is not a string in " + state_file.string());
            }
            std::string value = entry.at(key).get<std::string>();
            if (value.size() > kMaxFieldChars) {
                return Result<std::string>::failure(
                    std::string("job queue field \"") + key + "\" is " +
                    std::to_string(value.size()) + " characters, more than the " +
                    std::to_string(kMaxFieldChars) + " character limit in " +
                    state_file.string());
            }
            return Result<std::string>::success(std::move(value));
        };
        // The same for a field the file must carry.
        const auto required = [&](const nlohmann::json& entry,
                                  const char* key) -> Result<std::string> {
            if (!entry.contains(key)) {
                return Result<std::string>::failure(std::string("job queue row has no \"") + key +
                                                    "\" in " + state_file.string());
            }
            return bounded(entry, key, std::string());
        };

        uint64_t max_sequence = 0;
        for (const nlohmann::json& entry : root.at("jobs")) {
            if (!entry.is_object()) {
                return Result<JobQueue>::failure("job queue row is not an object in " +
                                                 state_file.string());
            }
            Job job;
            const Result<std::string> id = required(entry, "id");
            if (!id.ok()) return Result<JobQueue>::failure(id.message());
            job.id = id.value();
            const Result<std::string> photo = required(entry, "photo");
            if (!photo.ok()) return Result<JobQueue>::failure(photo.message());
            job.photo = Path(photo.value());
            const Result<std::string> backend = bounded(entry, "backend", std::string());
            if (!backend.ok()) return Result<JobQueue>::failure(backend.message());
            job.backend = backend.value();
            // Queues written before albums existed have no "album" key.
            const Result<std::string> album = bounded(entry, "album", std::string());
            if (!album.ok()) return Result<JobQueue>::failure(album.message());
            job.album = album.value();
            // Queues written before imports existed have no "kind" key.
            const Result<std::string> kind_name = bounded(entry, "kind", std::string("convert"));
            if (!kind_name.ok()) return Result<JobQueue>::failure(kind_name.message());
            const std::optional<JobKind> kind = job_kind_from_name(kind_name.value());
            if (!kind) {
                return Result<JobQueue>::failure("unknown job kind '" + kind_name.value() +
                                                 "' in " + state_file.string());
            }
            job.kind = *kind;
            job.rotate_quarters = entry.value("rotate", 0);
            // Three quarter turns either way is a whole rotation; anything else
            // is a corrupt field, not a rotation.
            if (job.rotate_quarters < -3 || job.rotate_quarters > 3) {
                return Result<JobQueue>::failure(
                    "job " + job.id + " has rotate " + std::to_string(job.rotate_quarters) +
                    ", outside -3..3, in " + state_file.string());
            }
            // Older queues have no destination override: they go to the
            // runner's local root.
            const Result<std::string> output_root = bounded(entry, "output_root", std::string());
            if (!output_root.ok()) return Result<JobQueue>::failure(output_root.message());
            if (!output_root.value().empty()) job.output_root = Path(output_root.value());
            // Queues written before stage H have no source folder: the row and
            // the scene's meta.json stay as they were (JobRunner derives one).
            const Result<std::string> source_folder = bounded(entry, "source_folder", std::string());
            if (!source_folder.ok()) return Result<JobQueue>::failure(source_folder.message());
            job.source_folder = source_folder.value();
            job.sequence = entry.value("sequence", uint64_t{0});
            // The id is job-<sequence>; a sequence past the id space (6 digits
            // in a live queue, far below this) is not one this title wrote. The
            // largest row is one below the limit so that its successor,
            // next_sequence, is still a value the reader accepts.
            if (job.sequence >= kMaxSequence) {
                return Result<JobQueue>::failure(
                    "job " + job.id + " has sequence " + std::to_string(job.sequence) +
                    ", not below " + std::to_string(kMaxSequence) + ", in " +
                    state_file.string());
            }
            const Result<std::string> message = bounded(entry, "message", std::string());
            if (!message.ok()) return Result<JobQueue>::failure(message.message());
            job.message = message.value();
            job.seconds_taken = entry.value("seconds_taken", 0.0);
            // A duration is finite and non-negative; a NaN or an infinity is a
            // corrupt field that would poison every comparison that reads it.
            if (!(job.seconds_taken >= 0.0) || job.seconds_taken > 1e9) {
                return Result<JobQueue>::failure("job " + job.id + " has an implausible duration in " +
                                                 state_file.string());
            }

            const Result<std::string> scene_folder = bounded(entry, "scene_folder", std::string());
            if (!scene_folder.ok()) return Result<JobQueue>::failure(scene_folder.message());
            if (!scene_folder.value().empty()) job.scene_folder = Path(scene_folder.value());

            const Result<std::string> state_name = required(entry, "state");
            if (!state_name.ok()) return Result<JobQueue>::failure(state_name.message());
            const std::optional<JobState> state = job_state_from_name(state_name.value());
            if (!state) {
                return Result<JobQueue>::failure("unknown job state '" + state_name.value() +
                                                 "' in " + state_file.string());
            }
            // A process that died mid-conversion cannot still be converting.
            job.state = *state == JobState::Running ? JobState::Queued : *state;

            if (job.id.empty()) {
                return Result<JobQueue>::failure("job queue contains a job without an id");
            }
            queue.jobs_.push_back(std::move(job));
            max_sequence = std::max(max_sequence, queue.jobs_.back().sequence);
        }

        const uint64_t stored_next = root.value("next_sequence", uint64_t{1});
        if (stored_next > kMaxSequence) {
            return Result<JobQueue>::failure(
                "job queue next_sequence " + std::to_string(stored_next) + " is more than " +
                std::to_string(kMaxSequence) + " in " + state_file.string());
        }
        // Every row's sequence is below kMaxSequence (checked above), so the
        // value written back is at most kMaxSequence: open() accepts a file
        // save() produced, including the one at the end of the id space.
        queue.next_sequence_ = std::max(stored_next, max_sequence + 1);
    } catch (const std::exception& parse_error) {
        return Result<JobQueue>::failure("invalid job queue JSON in " + state_file.string() + ": " +
                                         parse_error.what());
    }

    return Result<JobQueue>::success(std::move(queue));
}

JobQueue JobQueue::open_or_empty(const Path& state_file, std::string* notice) {
    if (notice != nullptr) notice->clear();
    Result<JobQueue> opened = open(state_file);
    if (opened.ok()) return opened.take();

    std::string reason = opened.message();
    // Keep the evidence: the operator can still inspect what the title refused,
    // and the empty queue that replaces it is written to a fresh file. A rename
    // that fails (a read-only folder) is not a second failure: the reason says
    // the file is still in place.
    vfs::FileStat inspected;
    if (vfs::stat(state_file, &inspected).ok() && inspected.exists) {
        const Path rejected = Path(state_file.string() + ".rejected");
        const Status moved = vfs::rename_file(state_file, rejected);
        if (moved.ok()) {
            reason += "; kept as " + rejected.string();
        } else {
            reason += "; could not move it aside (" + moved.message() + ")";
        }
    }
    if (notice != nullptr) *notice = reason;
    return empty(state_file);
}

JobQueue JobQueue::empty(const Path& state_file) {
    JobQueue queue;
    queue.state_file_ = state_file;
    return queue;
}

const Job* JobQueue::find(const std::string& id) const {
    for (const Job& job : jobs_) {
        if (job.id == id) return &job;
    }
    return nullptr;
}

Job* JobQueue::mutable_find(const std::string& id) {
    for (Job& job : jobs_) {
        if (job.id == id) return &job;
    }
    return nullptr;
}

Path JobQueue::normalize(const Path& photo) {
    // A location path is already its own absolute form ("smb0:/photos/x.jpg");
    // normal_absolute() would treat "smb0:" as a relative component and glue
    // the working directory in front of it.
    if (vfs::is_remote(photo)) return photo.lexically_normal();
    Path normalized;
    if (!normal_absolute(photo, normalized)) return Path();
    return normalized;
}

Result<std::string> JobQueue::enqueue(const Path& photo, std::string backend, std::string album,
                                     int rotate_quarters, const Path& output_root,
                                     std::string source_folder) {
    if (backend.empty()) return Result<std::string>::failure("backend name is empty");
    return add_job(photo, JobKind::convert, std::move(backend), std::move(album), rotate_quarters,
                   output_root, std::move(source_folder));
}

Result<std::string> JobQueue::enqueue_import(const Path& splat_file, std::string album,
                                             const Path& output_root,
                                             std::string source_folder) {
    return add_job(splat_file, JobKind::import, std::string(), std::move(album), 0, output_root,
                   std::move(source_folder));
}

Result<std::string> JobQueue::enqueue_rotate(const Path& scene_folder, int quarters,
                                            std::string album) {
    return add_job(scene_folder, JobKind::rotate, std::string(), std::move(album), quarters,
                   Path(), std::string());
}

Result<std::string> JobQueue::add_job(const Path& photo, JobKind kind, std::string backend,
                                      std::string album, int rotate_quarters,
                                      const Path& output_root, std::string source_folder) {
    if (photo.empty()) return Result<std::string>::failure("photo path is empty");
    // The reader's limits are also the writer's: a queue this build full of rows
    // it cannot read back would be discarded on the next boot. A full queue is
    // refused here, with the previous (readable) file still in place.
    if (jobs_.size() >= kMaxJobs) {
        return Result<std::string>::failure("job queue is full (" + std::to_string(jobs_.size()) +
                                            " row(s), limit " + std::to_string(kMaxJobs) + ")");
    }
    if (next_sequence_ >= kMaxSequence) {
        return Result<std::string>::failure(
            "job queue id space is exhausted (next sequence " + std::to_string(next_sequence_) +
            ", limit " + std::to_string(kMaxSequence) + ")");
    }

    const Path normalized = normalize(photo);
    // The lexical normal form is the only trusted answer: weakly_canonical()
    // returns "" with no error on a PS5 title when the path exists but
    // realpath() is refused (libc++ 18), which is exactly how the panel used to
    // enqueue an empty photo path.
    if (normalized.empty() || (!vfs::is_remote(normalized) && !normalized.is_absolute())) {
        return Result<std::string>::failure("photo path is empty");
    }
    for (const Job& existing : jobs_) {
        if (existing.photo != normalized) continue;
        if (existing.state == JobState::Queued || existing.state == JobState::Running) {
            return Result<std::string>::failure("photo is already queued: " + normalized.string());
        }
    }

    Job job;
    job.id = job_id_for(next_sequence_);
    job.photo = normalized;
    job.kind = kind;
    job.backend = std::move(backend);
    job.album = std::move(album);
    job.rotate_quarters = ((rotate_quarters % 4) + 4) % 4;
    // A location path is not an OS path: normal_absolute() would prepend the
    // working directory. It is a destination, not a source, so it is validated
    // by its grammar instead.
    job.output_root = output_root;
    job.source_folder = std::move(source_folder);
    job.state = JobState::Queued;
    job.sequence = next_sequence_;
    ++next_sequence_;
    jobs_.push_back(job);

    const Status saved = save();
    if (!saved.ok()) {
        jobs_.pop_back();
        --next_sequence_;
        return Result<std::string>::failure(saved.message());
    }
    return Result<std::string>::success(job.id);
}

Result<std::optional<Job>> JobQueue::take_next() {
    for (const Job& job : jobs_) {
        if (job.state == JobState::Running) return Result<std::optional<Job>>::success(std::nullopt);
    }

    Job* next = nullptr;
    for (Job& job : jobs_) {
        if (job.state != JobState::Queued) continue;
        if (next == nullptr || job.sequence < next->sequence) next = &job;
    }
    if (next == nullptr) return Result<std::optional<Job>>::success(std::nullopt);

    Job taken = *next;
    next->state = JobState::Running;
    const Status saved = save();
    if (!saved.ok()) {
        next->state = JobState::Queued;
        return Result<std::optional<Job>>::failure(saved.message());
    }
    return Result<std::optional<Job>>::success(std::optional<Job>(std::move(taken)));
}

Status JobQueue::mark_done(const std::string& id, const Path& scene_folder, double seconds_taken,
                          std::string message) {
    Job* job = mutable_find(id);
    if (job == nullptr) return Status::error("no such job: " + id);
    if (job->state != JobState::Running) {
        return Status::error("job " + id + " is " + job_state_name(job->state) + ", expected running");
    }

    job->state = JobState::Done;
    job->scene_folder = scene_folder;
    job->seconds_taken = seconds_taken;
    job->message = std::move(message);
    return save();
}

Status JobQueue::mark_failed(const std::string& id, std::string message) {
    Job* job = mutable_find(id);
    if (job == nullptr) return Status::error("no such job: " + id);
    if (job->state != JobState::Running) {
        return Status::error("job " + id + " is " + job_state_name(job->state) + ", expected running");
    }

    job->state = JobState::Failed;
    job->message = message.empty() ? std::string("unknown error") : std::move(message);
    return save();
}

Status JobQueue::cancel(const std::string& id) {
    Job* job = mutable_find(id);
    if (job == nullptr) return Status::error("no such job: " + id);
    if (job->state == JobState::Done || job->state == JobState::Failed ||
        job->state == JobState::Cancelled) {
        return Status::error("job " + id + " is already " + job_state_name(job->state));
    }

    job->state = JobState::Cancelled;
    job->message = "cancelled";
    return save();
}

Status JobQueue::save() const {
    nlohmann::json root;
    root["version"] = kQueueFormatVersion;
    root["next_sequence"] = next_sequence_;
    root["jobs"] = nlohmann::json::array();
    for (const Job& job : jobs_) {
        nlohmann::json entry;
        entry["id"] = job.id;
        entry["photo"] = job.photo.string();
        entry["kind"] = job_kind_name(job.kind);
        entry["backend"] = job.backend;
        entry["album"] = job.album;
        entry["rotate"] = job.rotate_quarters;
        if (!job.output_root.empty()) entry["output_root"] = job.output_root.string();
        if (!job.source_folder.empty()) entry["source_folder"] = job.source_folder;
        entry["state"] = job_state_name(job.state);
        entry["message"] = job.message;
        entry["scene_folder"] = job.scene_folder.string();
        entry["seconds_taken"] = job.seconds_taken;
        entry["sequence"] = job.sequence;
        root["jobs"].push_back(std::move(entry));
    }
    const std::string text = root.dump(2) + "\n";
    // The same limit open() checks from stat: never leave behind a file this
    // build would refuse to read. The caller's change is rolled back by
    // add_job(); the file on disk keeps its last readable state.
    if (text.size() > kMaxFileBytes) {
        return Status::error("job queue would be " + std::to_string(text.size()) +
                             " bytes, more than the " + std::to_string(kMaxFileBytes) +
                             " byte limit");
    }

    const Path parent = state_file_.parent_path();
    if (!parent.empty()) {
        const Status made = vfs::make_dirs(parent);
        if (!made.ok()) return made;
    }

    std::string write_error;
    if (!vfs::write_text_atomically(state_file_, text, &write_error)) {
        return Status::error(write_error);
    }
    return Status::success();
}

}  // namespace splat
