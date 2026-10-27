#include "jobs/job_queue.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace splat {
namespace {

constexpr int kQueueFormatVersion = 1;

std::string job_id_for(uint64_t sequence) {
    std::ostringstream out;
    out << "job-" << std::setw(6) << std::setfill('0') << sequence;
    return out.str();
}

// Flushes a directory entry so a rename survives a power cut, not just a
// process crash. Best effort: a filesystem that cannot do it still gets the
// atomic rename.
void sync_directory(const Path& directory) {
#if !defined(_WIN32)
    if (directory.empty()) return;
    const int fd = ::open(directory.string().c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#else
    (void)directory;
#endif
}

// Writes text to `path` and flushes it to the device, so the following rename
// publishes complete data even if the console loses power.
bool write_file_durably(const Path& path, const std::string& text, std::string& error) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        error = "cannot open " + path.string() + " for writing";
        return false;
    }
    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    bool ok = written == text.size();
    if (!ok) error = "failed while writing " + path.string();
    if (std::fflush(file) != 0) {
        ok = false;
        if (error.empty()) error = "failed while flushing " + path.string();
    }
#if !defined(_WIN32)
    // fflush only reaches the OS; fsync reaches the device, which is what makes
    // the rename below safe across a power cut.
    if (::fsync(::fileno(file)) != 0) {
        ok = false;
        if (error.empty()) error = "cannot sync " + path.string();
    }
#endif
    if (std::fclose(file) != 0) {
        ok = false;
        if (error.empty()) error = "failed while closing " + path.string();
    }
    return ok;
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

Result<JobQueue> JobQueue::open(const Path& state_file) {
    JobQueue queue;
    queue.state_file_ = state_file;

    std::error_code error;
    if (!std::filesystem::exists(state_file, error)) {
        if (error) {
            return Result<JobQueue>::failure("cannot inspect " + state_file.string() + ": " +
                                             error.message());
        }
        return Result<JobQueue>::success(std::move(queue));
    }

    std::ifstream file(state_file, std::ios::binary);
    if (!file) return Result<JobQueue>::failure("cannot open " + state_file.string());
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object()) return Result<JobQueue>::failure("job queue root is not an object");

        uint64_t max_sequence = 0;
        for (const nlohmann::json& entry : root.at("jobs")) {
            Job job;
            job.id = entry.at("id").get<std::string>();
            job.photo = Path(entry.at("photo").get<std::string>());
            job.backend = entry.value("backend", std::string());
            // Queues written before albums existed have no "album" key.
            job.album = entry.value("album", std::string());
            job.sequence = entry.value("sequence", uint64_t{0});
            job.message = entry.value("message", std::string());
            job.seconds_taken = entry.value("seconds_taken", 0.0);

            const std::string scene_folder = entry.value("scene_folder", std::string());
            if (!scene_folder.empty()) job.scene_folder = Path(scene_folder);

            const std::string state_name = entry.at("state").get<std::string>();
            const std::optional<JobState> state = job_state_from_name(state_name);
            if (!state) {
                return Result<JobQueue>::failure("unknown job state '" + state_name + "' in " +
                                                 state_file.string());
            }
            // A process that died mid-conversion cannot still be converting.
            job.state = *state == JobState::Running ? JobState::Queued : *state;

            if (job.id.empty()) {
                return Result<JobQueue>::failure("job queue contains a job without an id");
            }
            queue.jobs_.push_back(std::move(job));
            max_sequence = std::max(max_sequence, queue.jobs_.back().sequence);
        }

        queue.next_sequence_ = std::max(root.value("next_sequence", uint64_t{1}), max_sequence + 1);
    } catch (const std::exception& parse_error) {
        return Result<JobQueue>::failure("invalid job queue JSON in " + state_file.string() + ": " +
                                         parse_error.what());
    }

    return Result<JobQueue>::success(std::move(queue));
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
    Path normalized;
    if (!normal_absolute(photo, normalized)) return Path();
    return normalized;
}

Result<std::string> JobQueue::enqueue(const Path& photo, std::string backend, std::string album) {
    if (photo.empty()) return Result<std::string>::failure("photo path is empty");
    if (backend.empty()) return Result<std::string>::failure("backend name is empty");

    const Path normalized = normalize(photo);
    // The lexical normal form is the only trusted answer: weakly_canonical()
    // returns "" with no error on a PS5 title when the path exists but
    // realpath() is refused (libc++ 18), which is exactly how the panel used to
    // enqueue an empty photo path.
    if (normalized.empty() || !normalized.is_absolute()) {
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
    job.backend = std::move(backend);
    job.album = std::move(album);
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

Status JobQueue::mark_done(const std::string& id, const Path& scene_folder, double seconds_taken) {
    Job* job = mutable_find(id);
    if (job == nullptr) return Status::error("no such job: " + id);
    if (job->state != JobState::Running) {
        return Status::error("job " + id + " is " + job_state_name(job->state) + ", expected running");
    }

    job->state = JobState::Done;
    job->scene_folder = scene_folder;
    job->seconds_taken = seconds_taken;
    job->message.clear();
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
        entry["backend"] = job.backend;
        entry["album"] = job.album;
        entry["state"] = job_state_name(job.state);
        entry["message"] = job.message;
        entry["scene_folder"] = job.scene_folder.string();
        entry["seconds_taken"] = job.seconds_taken;
        entry["sequence"] = job.sequence;
        root["jobs"].push_back(std::move(entry));
    }
    const std::string text = root.dump(2) + "\n";

    std::error_code error;
    const Path parent = state_file_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return Status::error("cannot create " + parent.string() + ": " + error.message());
    }

    Path temporary = state_file_;
    temporary += ".tmp";
    std::string write_error;
    if (!write_file_durably(temporary, text, write_error)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Status::error(write_error);
    }

    std::filesystem::rename(temporary, state_file_, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Status::error("cannot replace " + state_file_.string() + ": " + error.message());
    }
    sync_directory(parent);
    return Status::success();
}

}  // namespace splat
