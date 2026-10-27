#include "app_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "common/bytes.h"
#include "ingest/lan_address.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/scene_folder.h"

namespace splat {
namespace {

constexpr const char* kAppTag = "[app]";

// Resident set of this process. On both targets the GPU memory is part of it
// (unified memory: Apple silicon, PS5), so it is the honest fallback when the
// platform cannot report a GPU heap budget.
uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.resident_size;
#elif defined(__linux__)
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    if (file == nullptr) return 0;
    unsigned long size = 0;
    unsigned long resident = 0;
    const int fields = std::fscanf(file, "%lu %lu", &size, &resident);
    std::fclose(file);
    if (fields != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

uint64_t peak_resident_bytes() {
#if !defined(_WIN32)
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ull;
#endif
#else
    return 0;
#endif
}

const char* phase_name(GpuPhase phase) {
    switch (phase) {
        case GpuPhase::Idle: return "idle";
        case GpuPhase::Inference: return "inference";
        case GpuPhase::Viewing: return "viewing";
    }
    return "unknown";
}

// "2026-10-26T05:12:33.456Z": sortable, UTC, milliseconds, no locale.
std::string timestamp_iso_ms() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[40];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(milliseconds.count()));
    return text;
}

bool has_queued_job(const JobQueue& queue) {
    for (const Job& job : queue.jobs()) {
        if (job.state == JobState::Queued || job.state == JobState::Running) return true;
    }
    return false;
}

}  // namespace

AppMode::AppMode(AppModeOptions options) : options_(std::move(options)) {
    data_dir_ = options_.data_dir;
    if (data_dir_.empty()) data_dir_ = Path(".");
    started_ = false;
}

AppMode::~AppMode() { stop(); }

Path AppMode::inbox_dir() const { return data_dir_ / "inbox"; }
Path AppMode::scenes_dir() const { return data_dir_ / "scenes"; }
Path AppMode::state_dir() const { return data_dir_ / "state"; }
Path AppMode::models_dir() const { return data_dir_ / "models"; }

const std::string& AppMode::pin() const {
    static const std::string kEmpty;
    return server_ ? server_->pin() : kEmpty;
}

std::string AppMode::upload_url() const {
    if (!server_ || !server_->running()) return std::string();
    return "http://" + server_->bind_address() + ":" + std::to_string(server_->port()) + "/";
}

std::string AppMode::window_title() const {
    const std::string url = upload_url();
    if (url.empty()) return "Photo Splat VR (app mode)";
    return "Photo Splat VR  " + url + "  PIN " + pin();
}

QueueSnapshot AppMode::queue_snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

double AppMode::seconds_since_start() const {
    if (start_time_.time_since_epoch().count() == 0) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
}

void AppMode::log_transition(const std::string& event, const std::string& detail) {
    log_line(event, detail, guard_.phase(), guard_.model_loaded());
}

void AppMode::log_line(const std::string& event, const std::string& detail, GpuPhase phase,
                       bool model_loaded) {
    std::string line = timestamp_iso_ms();
    line += ' ';
    line += event;
    if (!detail.empty()) {
        line += ' ';
        line += detail;
    }
    line += " phase=";
    line += phase_name(phase);
    line += " model=";
    line += model_loaded ? "1" : "0";
    line += " rss=";
    line += human_bytes(resident_bytes());
    line += " peak=";
    line += human_bytes(peak_resident_bytes());
    line += " gpu=";
    line += options_.gpu_memory ? options_.gpu_memory() : std::string("n/a");

    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_.is_open()) {
            log_ << line << '\n';
            log_.flush();
        }
    }
    std::printf("%s %s\n", kAppTag, line.c_str());
    std::fflush(stdout);
}

bool AppMode::verify_model_file(const std::string& manager_backend, const Path& path,
                               std::string* note) const {
    if (models_ == nullptr) return false;
    const Status verified = models_->verify_gguf(manager_backend, path);
    if (note != nullptr) *note = verified.message();
    return verified.ok();
}

Status AppMode::resolve_backend() {
    const std::string requested = options_.backend;
    if (requested != "auto" && requested != kSharpBackend && requested != kFallbackBackend) {
        return Status::error("unknown backend '" + requested + "' (auto, sharp or fallback)");
    }

    const std::string sharp_manager = sharp_model_spec().name;  // "sharp"
    const std::string da2_manager = da2_model_spec().name;      // "da2"

    // A backend is usable when the model manager's record says Ready, or when a
    // converted GGUF with the pinned metadata sits in <data>/models/. The
    // second path is the "copy or symlink the weights into the data dir" route.
    auto usable = [&](const std::string& manager_name, const Path& local_gguf, Path* out,
                      std::string* note) -> bool {
        const Result<ModelRecord> record = models_->status(manager_name);
        if (record.ok() && record.value().state == ModelState::Ready) {
            *out = record.value().gguf_path;
            *note = "model manager reports ready";
            return true;
        }
        std::error_code error;
        if (std::filesystem::exists(local_gguf, error) && !error) {
            std::string verify_note;
            if (verify_model_file(manager_name, local_gguf, &verify_note)) {
                *out = local_gguf;
                *note = "verified " + local_gguf.string();
                return true;
            }
            *note = local_gguf.string() + " rejected: " + verify_note;
            return false;
        }
        *note = record.ok() ? "model manager: " + std::string(model_state_name(record.value().state))
                            : "model manager: " + record.message();
        return false;
    };

    Path sharp_path;
    std::string sharp_note;
    const bool sharp_usable = usable(sharp_manager, models_dir() / "sharp.gguf", &sharp_path,
                                     &sharp_note);
    Path fallback_path;
    std::string fallback_note;
    const bool fallback_usable = usable(da2_manager, models_dir() / "da2.gguf", &fallback_path,
                                        &fallback_note);

    // --model names the file the session converts with, so it decides the
    // backend on its own when it carries recognisable metadata.
    if (!options_.model_path.empty()) {
        std::error_code error;
        if (!std::filesystem::exists(options_.model_path, error) || error) {
            return Status::error("model file does not exist: " + options_.model_path.string());
        }
        if (requested == kSharpBackend) {
            backend_ = kSharpBackend;
        } else if (requested == kFallbackBackend) {
            backend_ = kFallbackBackend;
        } else {
            std::string note;
            backend_ = verify_model_file(sharp_manager, options_.model_path, &note) ? kSharpBackend
                                                                                   : kFallbackBackend;
        }
        model_path_ = options_.model_path;
        backend_note_ = "--model " + model_path_.string();
        return Status::success();
    }

    if (requested == kSharpBackend) {
        if (!sharp_usable) {
            return Status::error("sharp backend requested but no converted SHARP GGUF is ready (" +
                                 sharp_note + ")");
        }
        backend_ = kSharpBackend;
        model_path_ = sharp_path;
        backend_note_ = sharp_note;
    } else if (requested == kFallbackBackend) {
        if (!fallback_usable) {
            return Status::error("fallback backend requested but no converted DA2 GGUF is ready (" +
                                 fallback_note + ")");
        }
        backend_ = kFallbackBackend;
        model_path_ = fallback_path;
        backend_note_ = fallback_note;
    } else if (sharp_usable) {
        backend_ = kSharpBackend;
        model_path_ = sharp_path;
        backend_note_ = "auto: " + sharp_note;
    } else if (fallback_usable) {
        backend_ = kFallbackBackend;
        model_path_ = fallback_path;
        backend_note_ = "auto: sharp not ready (" + sharp_note + "), " + fallback_note;
    } else {
        return Status::error(
            "no reconstruction backend is ready: sharp (" + sharp_note + ") and da2 (" +
            fallback_note + "); put a converted GGUF in " + models_dir().string() +
            " or convert one with tools/model_cli");
    }
    return Status::success();
}

void AppMode::refresh_queue_snapshot() {
    QueueSnapshot snapshot;
    if (queue_) {
        for (const Job& job : queue_->jobs()) {
            QueueEntry entry;
            entry.id = job.id;
            entry.photo = job.photo.filename().string();
            entry.state = job_state_name(job.state);
            entry.message = job.message;
            entry.seconds_taken = job.seconds_taken;
            snapshot.push_back(std::move(entry));
        }
    }
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = std::move(snapshot);
}

void AppMode::refresh_playlist_locked() {
    Result<ScenePlaylist> opened = ScenePlaylist::from_folder(scenes_dir());
    ScenePlaylist fresh;
    if (opened.ok()) fresh = opened.take();

    // Newest first: scene.psplat mtime descending, folder name descending as
    // the tie-break (job ids are zero padded, so that is job order).
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> keys;
    keys.reserve(fresh.size());
    for (size_t index = 0; index < fresh.size(); ++index) {
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(fresh.entry(index).scene_file, error);
        keys.emplace_back(error ? std::filesystem::file_time_type{} : stamp,
                          fresh.entry(index).folder.filename().string());
    }
    std::vector<size_t> order(fresh.size());
    for (size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [&keys](size_t a, size_t b) {
        if (keys[a].first != keys[b].first) return keys[a].first > keys[b].first;
        return keys[a].second > keys[b].second;
    });

    std::vector<std::string> previous_names;
    for (size_t index : order_) {
        previous_names.push_back(playlist_.entry(index).folder.filename().string());
    }
    std::vector<std::string> current_names;
    for (size_t index : order) {
        current_names.push_back(fresh.entry(index).folder.filename().string());
    }

    playlist_ = std::move(fresh);
    order_ = std::move(order);

    if (previous_names == current_names) return;

    ++generation_;
    current_ = 0;  // a new scene becomes the one on screen
    if (current_names.empty()) {
        log_transition("app.playlist", "empty");
        return;
    }
    log_transition("app.playlist", "scenes=" + std::to_string(current_names.size()) +
                                       " newest=" + current_names.front());
}

uint64_t AppMode::scene_generation() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    return generation_;
}

size_t AppMode::scene_count() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    return order_.size();
}

std::optional<PlaylistEntry> AppMode::scene(size_t browse_index) const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (browse_index >= order_.size()) return std::nullopt;
    return playlist_.entry(order_[browse_index]);
}

std::optional<PlaylistEntry> AppMode::current_scene() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (order_.empty()) return std::nullopt;
    return playlist_.entry(order_[current_]);
}

size_t AppMode::current_index() const {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    return order_.empty() ? 0 : current_;
}

bool AppMode::select(size_t browse_index) {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (browse_index >= order_.size()) return false;
    current_ = browse_index;
    return true;
}

void AppMode::next() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (order_.empty()) return;
    current_ = (current_ + 1) % order_.size();
}

void AppMode::previous() {
    std::lock_guard<std::mutex> lock(playlist_mutex_);
    if (order_.empty()) return;
    current_ = (current_ + order_.size() - 1) % order_.size();
}

bool AppMode::try_begin_viewing() {
    if (work_pending()) return false;
    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        if (order_.empty()) return false;
    }
    return guard_.try_begin_viewing();
}

void AppMode::end_viewing() { guard_.end_viewing(); }

void AppMode::set_work_pending(bool pending) {
    work_pending_.store(pending);
    release_requested_.store(pending);
    wait_cv_.notify_all();
}

bool AppMode::wait_for_jobs(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    return wait_cv_.wait_for(lock, timeout, [this, count] {
        return finished_jobs_.load() >= count || stop_.load();
    });
}

void AppMode::worker_main() {
    std::string last_stage;
    while (!stop_.load()) {
        for (const Path& photo : watcher_->poll()) {
            Result<std::string> enqueued = queue_->enqueue(photo, backend_);
            if (!enqueued.ok()) {
                log_transition("app.enqueue.failed",
                               "photo=" + photo.filename().string() + " error=" +
                                   enqueued.message());
            } else {
                log_transition("app.enqueue", "job=" + enqueued.value() +
                                                  " photo=" + photo.filename().string() +
                                                  " backend=" + backend_);
            }
        }

        refresh_queue_snapshot();
        if (has_queued_job(*queue_)) {
            set_work_pending(true);
            log_transition("app.inference.request", "releasing the viewer");

            Progress progress([this, &last_stage](float fraction, const std::string& stage) {
                // Same thread as run_next, so touching the queue here is safe.
                refresh_queue_snapshot();
                if (stage == last_stage) return;
                last_stage = stage;
                log_transition("app.job.stage", "stage=" + stage +
                                                    " fraction=" + std::to_string(fraction));
            });

            const auto job_started = std::chrono::steady_clock::now();
            Result<std::optional<JobOutcome>> outcome = runner_->run_next(
                [&progress](float fraction, const std::string& stage) {
                    progress.report(fraction, stage);
                });
            const double job_seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - job_started)
                    .count();
            last_stage.clear();
            release_requested_.store(false);

            if (!outcome.ok()) {
                log_transition("app.job.error", "error=" + outcome.message());
            } else if (!outcome.value()) {
                log_transition("app.job.none", "nothing ran");
            } else {
                const JobOutcome& result = outcome.value().value();
                if (result.state == JobState::Done) {
                    std::string detail = "job=" + result.job_id +
                                         " seconds=" + std::to_string(result.seconds_taken) +
                                         " wall=" + std::to_string(job_seconds);
                    Result<SceneMeta> meta =
                        read_scene_meta(scene_files(result.scene_folder).meta);
                    if (meta.ok()) {
                        detail += " splats=" + std::to_string(meta.value().splat_count) +
                                  " backend=" + meta.value().backend;
                    }
                    detail += " folder=" + result.scene_folder.filename().string();
                    log_transition("app.job.done", detail);
                    if (const Job* finished = queue_->find(result.job_id)) {
                        archive_inbox_photo(finished->photo);
                    }
                } else {
                    log_transition("app.job.failed", "job=" + result.job_id +
                                                         " state=" + job_state_name(result.state) +
                                                         " message=" + result.message);
                }
            }

            // Publish the new scene before signalling the job as finished, so a
            // waiter that sees finished_jobs() also sees the playlist entry.
            {
                std::lock_guard<std::mutex> lock(playlist_mutex_);
                refresh_playlist_locked();
            }
            refresh_queue_snapshot();

            if (outcome.ok() && outcome.value()) {
                ++finished_jobs_;
                wait_cv_.notify_all();
            }
        }

        if (!has_queued_job(*queue_)) set_work_pending(false);

        std::unique_lock<std::mutex> lock(wait_mutex_);
        wait_cv_.wait_for(lock, std::chrono::milliseconds(std::max(1, options_.poll_interval_ms)),
                          [this] { return stop_.load(); });
    }
}

Status AppMode::start() {
    if (started_) return Status::error("app mode is already started");

    std::error_code error;
    for (const Path& directory :
         {inbox_dir(), scenes_dir(), state_dir(), models_dir()}) {
        std::filesystem::create_directories(directory, error);
        if (error) {
            return Status::error("cannot create " + directory.string() + ": " + error.message());
        }
    }

    start_time_ = std::chrono::steady_clock::now();
    log_path_ = options_.transition_log.empty() ? state_dir() / "transitions.log"
                                                : options_.transition_log;
    if (!log_path_.parent_path().empty()) {
        std::filesystem::create_directories(log_path_.parent_path(), error);
    }
    log_.open(log_path_, std::ios::app);
    if (!log_.is_open()) {
        return Status::error("cannot open the transition log " + log_path_.string());
    }
    log_transition("app.start", "data=" + data_dir_.string() + " log=" + log_path_.string());

    Result<JobQueue> queue = JobQueue::open(state_dir() / "queue.json");
    if (!queue.ok()) return Status::error(queue.message());
    queue_ = std::make_unique<JobQueue>(queue.take());

    ModelManagerOptions model_options;
    model_options.state_dir = state_dir();
    models_ = std::make_unique<ModelManager>(std::move(model_options));

    const Status resolved = resolve_backend();
    if (!resolved.ok()) {
        log_transition("app.backend.failed", "error=" + resolved.message());
        return resolved;
    }
    log_transition("app.backend", "backend=" + backend_ + " model=" + model_path_.string() +
                                      " reason=" + backend_note_);

    // Every phase change goes into the same log, so the order of "model
    // unloaded" and "viewer begin" can be checked after the fact. The guard
    // reports the state it had when it made the transition and calls this under
    // its own lock, so log_transition() must not touch the guard here: the line
    // is written from the snapshot.
    guard_.set_observer([this](GpuTransition transition, GpuPhase phase, bool model_loaded) {
        log_line(gpu_transition_name(transition), std::string(), phase, model_loaded);
    });

    // The page address is resolved here so the loopback fallback is an explicit
    // choice: IngestServer would otherwise pick 127.0.0.1 by itself, but a
    // session that cannot be reached from a phone should say so instead of
    // silently serving a local-only page. Nothing here can produce 0.0.0.0 or a
    // public address: detect_lan_address() only returns private IPv4, and
    // IngestServer rejects everything but private IPv4 and loopback.
    std::string bind_address = options_.bind_address;
    if (bind_address.empty()) {
        bind_address = detect_lan_address();
        if (bind_address.empty()) {
            log_transition("app.server.failed", "error=no private LAN address on this host");
            return Status::error(
                "no private LAN address on this host: pass --bind 127.0.0.1 to serve "
                "the upload page on loopback only");
        }
    }

    IngestServerConfig config;
    config.inbox = inbox_dir();
    config.bind_address = bind_address;
    config.port = options_.port;
    config.queue = [this] { return queue_snapshot(); };
    server_ = std::make_unique<IngestServer>(config);
    const Status served = server_->start();
    if (!served.ok()) {
        log_transition("app.server.failed", "error=" + served.message());
        return served;
    }

    watcher_ = std::make_unique<InboxWatcher>(std::vector<Path>{inbox_dir()});

    JobRunnerConfig runner_config;
    runner_config.output_root = scenes_dir();
    runner_config.model_path = model_path_;
    runner_ = std::make_unique<JobRunner>(
        *queue_, guard_,
        [this](const std::string& name) { return make_backend(name); }, runner_config);

    {
        std::lock_guard<std::mutex> lock(playlist_mutex_);
        refresh_playlist_locked();
    }
    refresh_queue_snapshot();

    std::printf("%s upload page %s   PIN %s\n", kAppTag, upload_url().c_str(), pin().c_str());
    std::printf("%s backend %s  model %s\n", kAppTag, backend_.c_str(),
                model_path_.string().c_str());
    std::fflush(stdout);

    worker_ = std::thread(&AppMode::worker_main, this);
    started_ = true;
    return Status::success();
}

void AppMode::stop() {
    if (!started_) return;
    if (server_) server_->stop();
    stop_.store(true);
    wait_cv_.notify_all();
    // Always shut the guard down, whether or not a job holds the GPU right now:
    // it releases a worker that is parked in begin_inference() waiting for the
    // viewer, and a job that is already running still finishes and logs its
    // own end/unload afterwards. Shutting it down only after the join would
    // deadlock against that parked worker.
    guard_.shutdown();
    worker_.join();
    log_transition("app.stop", "seconds=" + std::to_string(seconds_since_start()));
    started_ = false;
}

void AppMode::archive_inbox_photo(const Path& photo) {
    std::error_code error;
    // The queue stores the lexical form of the photo path (no realpath: a PS5
    // title is refused it), so the inbox must be compared in the same form.
    Path inbox;
    if (!normal_absolute(inbox_dir(), inbox)) return;
    if (photo.parent_path() != inbox) return;  // a subfolder or a USB drive

    const Path archive = state_dir() / "ingested";
    std::filesystem::create_directories(archive, error);
    if (error) return;
    const Path target = archive / photo.filename();
    std::filesystem::rename(photo, target, error);
    if (error) {
        log_transition("app.inbox.archive_failed",
                       "photo=" + photo.filename().string() + " error=" + error.message());
        return;
    }
    log_transition("app.inbox.archived", "photo=" + photo.filename().string());
}

std::unique_ptr<IReconstructor> AppMode::make_backend(const std::string& requested) {
    if (requested != backend_) {
        log_transition("app.job.backend_override",
                       "job_backend=" + requested + " using=" + backend_);
    }
    if (backend_ == kSharpBackend) {
        SharpConfig config;
        config.prefer_vulkan = options_.prefer_vulkan;
        return std::unique_ptr<IReconstructor>(new SharpReconstructor(config));
    }
    FallbackConfig config;
    config.prefer_vulkan = options_.prefer_vulkan;
    return std::unique_ptr<IReconstructor>(new FallbackReconstructor(config));
}

}  // namespace splat
