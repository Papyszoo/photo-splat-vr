#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "ingest/ingest_server.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/inbox_watcher.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "model/model_manager.h"
#include "recon/reconstructor.h"
#include "render/scene_playlist.h"

namespace splat {

// Backend names as the queue and the runner factory know them. `sharp` is the
// quality backend; `fallback` is C4 (Depth Anything V2 Small).
inline constexpr const char* kSharpBackend = "sharp";
inline constexpr const char* kFallbackBackend = "fallback";

// Everything the session needs to know before it can start. Paths stay relative
// to the data directory the caller names; nothing here is baked into the code.
struct AppModeOptions {
    Path data_dir;  // --app: inbox/, scenes/, state/ and models/ live here
    // "auto" picks sharp when its converted GGUF is ready, otherwise fallback.
    std::string backend = "auto";
    // Explicit converted GGUF. Empty: <data>/models/<backend>.gguf, then the
    // ModelManager's ready record.
    Path model_path;
    // Empty: the LAN address from core/ingest/lan_address.h. There is no
    // implicit loopback fallback: on a host without a private LAN interface
    // start() fails and asks for --bind 127.0.0.1 (or another loopback
    // address), so binding locally is always an explicit choice.
    std::string bind_address;
    uint16_t port = 8090;  // 0 picks a free port (tests)
    int poll_interval_ms = 250;  // inbox / queue polling period
    // Empty: <state>/transitions.log. One line per GPU phase change and per
    // job event, so a run can be audited afterwards.
    Path transition_log;
    // Passed to the reconstructors; false forces the CPU backend (tests, CI).
    bool prefer_vulkan = true;
    // Optional GPU memory probe for the transition log. The desktop app passes
    // a Vulkan heap-budget query; without one the log says gpu=n/a.
    std::function<std::string()> gpu_memory;
};

// D1 desktop app mode: the photo-to-VR flow with the C9 pipeline as the only
// producer of scenes.
//
//   upload (LAN page + PIN) -> inbox -> watcher -> queue -> runner (C2 backend)
//   -> C5 scene folder -> playlist -> viewer
//
// Inference and viewing take turns on the GPU through the existing
// GpuPhaseGuard: the worker asks the viewer to release, the guard blocks
// inference until the viewer is out, and the viewer only re-allocates after the
// runner reported the model unloaded. The session owns the guard, the queue,
// the runner, the HTTP page and the playlist; the caller owns the GPU objects
// (window, Vulkan context, renderer) and talks to the session through the
// viewer-side calls below, which never touch Vulkan themselves.
class AppMode {
public:
    explicit AppMode(AppModeOptions options);
    ~AppMode();

    AppMode(const AppMode&) = delete;
    AppMode& operator=(const AppMode&) = delete;

    // Creates the directories, opens the queue and the transition log, resolves
    // the backend, starts the LAN page and the worker thread.
    Status start();
    // Stops the page, then the guard (so a worker parked waiting for the GPU
    // gives up and a job that is already running still finishes), then the
    // worker. `guard.shutdown` is always logged and the guard stays stopped.
    void stop();

    // Layout -----------------------------------------------------------------
    const Path& data_dir() const { return data_dir_; }
    Path inbox_dir() const;
    Path scenes_dir() const;
    Path state_dir() const;
    Path models_dir() const;
    const Path& transition_log_path() const { return log_path_; }

    // Backend -----------------------------------------------------------------
    // "sharp" or "fallback": the backend this session converts with.
    const std::string& backend() const { return backend_; }
    const Path& model_path() const { return model_path_; }
    // One line: why auto chose this backend (shown at start-up and logged).
    const std::string& backend_note() const { return backend_note_; }

    // LAN page ----------------------------------------------------------------
    const std::string& pin() const;
    // The address the page bound to and its port (0 before start()).
    std::string bind_address() const { return server_ ? server_->bind_address() : std::string(); }
    uint16_t port() const { return server_ ? server_->port() : 0; }
    // http://<bind address>:<port>/ ; empty before start().
    std::string upload_url() const;
    // "<upload url>   PIN <pin>" for the window title.
    std::string window_title() const;
    QueueSnapshot queue_snapshot() const;

    // Viewer side (no Vulkan; the caller owns the GPU objects) -----------------
    // True while the worker wants the GPU for inference.
    bool release_requested() const { return release_requested_.load(); }
    // A queued or running job keeps the viewer out.
    bool work_pending() const { return work_pending_.load(); }
    // Non-blocking. Acquires the viewing phase when no job is pending and at
    // least one scene exists. Every successful call needs end_viewing().
    bool try_begin_viewing();
    void end_viewing();

    // Scenes ------------------------------------------------------------------
    // Bumped whenever the scene list changes (a job finished, or a folder
    // appeared or disappeared).
    uint64_t scene_generation() const;
    size_t scene_count() const;
    // Browse order: 0 is the newest scene. Empty when no scene is known.
    std::optional<PlaylistEntry> scene(size_t browse_index) const;
    std::optional<PlaylistEntry> current_scene() const;
    size_t current_index() const;
    bool select(size_t browse_index);
    void next();
    void previous();

    // Jobs --------------------------------------------------------------------
    // Jobs that reached a terminal state since start(). Poll with wait_for_jobs.
    size_t finished_jobs() const { return finished_jobs_.load(); }
    // Waits until at least `count` jobs finished, or the timeout expires.
    bool wait_for_jobs(size_t count, std::chrono::milliseconds timeout);

    // Transition log ----------------------------------------------------------
    // Timestamped line with the guard state and the measured GPU memory. Safe
    // from any thread; also used by the viewer for its own events.
    void log_transition(const std::string& event, const std::string& detail = std::string());

    // True once stop() shut the phase guard down.
    bool phase_guard_stopped() const { return guard_.stopped(); }

    // Time since start(), for log lines and reports.
    double seconds_since_start() const;

private:
    void worker_main();
    void refresh_playlist_locked();
    void refresh_queue_snapshot();
    void set_work_pending(bool pending);
    // Writes one line using the caller's snapshot of the guard state, so the
    // transition observer does not have to re-query the guard while it holds
    // the guard's lock.
    void log_line(const std::string& event, const std::string& detail, GpuPhase phase,
                  bool model_loaded);
    // Builds the reconstructor for a job. The session converts with one backend
    // for its whole life; a job recorded with another name is converted with
    // this one and the override is logged.
    std::unique_ptr<IReconstructor> make_backend(const std::string& requested);
    // Moves a converted photo out of the inbox, so a restart does not convert
    // the same file again. Only files directly in the inbox are moved.
    void archive_inbox_photo(const Path& photo);
    // Resolves backend_ / model_path_ / backend_note_.
    Status resolve_backend();
    // Verifies a converted GGUF the way ModelManager does, without requiring
    // the file to sit in its own models directory.
    bool verify_model_file(const std::string& manager_backend, const Path& path,
                           std::string* note) const;

    AppModeOptions options_;
    Path data_dir_;
    Path log_path_;
    std::unique_ptr<ModelManager> models_;
    std::unique_ptr<IngestServer> server_;
    std::unique_ptr<JobQueue> queue_;
    std::unique_ptr<JobRunner> runner_;
    GpuPhaseGuard guard_;
    std::thread worker_;
    std::unique_ptr<InboxWatcher> watcher_;

    std::string backend_;
    Path model_path_;
    std::string backend_note_;

    // Transition log: one mutex, one file, appended from several threads.
    mutable std::mutex log_mutex_;
    std::ofstream log_;
    std::chrono::steady_clock::time_point start_time_{};

    // Scene list, newest first. Rebuilt by the worker, read by the viewer.
    mutable std::mutex playlist_mutex_;
    ScenePlaylist playlist_;
    std::vector<size_t> order_;  // browse index -> playlist entry index
    size_t current_ = 0;
    uint64_t generation_ = 0;

    // Queue state for readers. The worker owns queue_; everyone else reads the
    // snapshot (the LAN page polls it once a second).
    mutable std::mutex snapshot_mutex_;
    QueueSnapshot snapshot_;

    std::atomic<bool> release_requested_{false};
    std::atomic<bool> work_pending_{false};
    std::atomic<size_t> finished_jobs_{0};

    std::atomic<bool> stop_{false};
    mutable std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool started_ = false;
};

}  // namespace splat
