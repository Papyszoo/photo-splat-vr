// The panel's adapter over the C1 model manager (core/model/model_manager.h).
//
// It turns the manager's persisted records into the rows the Models screen
// draws, and owns the one long-running operation the UI needs: a download (plus
// verify and convert) on a worker thread, with progress and a cancel request.
// The manager still does the licence gate, the resumable transfer, the pinned
// SHA-256 check and the conversion; this class never talks to the network
// itself.
//
// Only SHARP and DA2-Small are offered: they are the pinned entries. There is no
// default model, so selected() is empty until the user selects a Ready row, and
// the choice lives in state/ui.json through the shared UiState.
//
// Threads: the download worker, the stall watch and the licence fetch. At Quit
// the host calls stop(), which signals and waits at most kQuitWaitSeconds; when
// it returns false a thread is still inside a fetcher call and the host leaves
// the whole panel to the process exit instead of destroying this object under
// it (see ~ModelService).
//
// I/O: list() serves a snapshot built on the workers (download events, the
// stall watch's tick-driven refresh, the constructor); it never opens, stats or
// parses a file on the render thread. That was the per-frame /data cost of
// runs/issue-1-crash/sharp-slow-report.md §3.3.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "model/model_manager.h"
#include "ui/download_eta.h"
#include "ui/services.h"
#include "ui/ui_state.h"

namespace splat {

// A licence_text() result that is a message rather than a licence starts with
// this prefix; the Licence screen shows the rest and offers no acceptance.
inline constexpr char kLicenceUnavailablePrefix[] = "Licence unavailable: ";

// licence_text() while its fetch runs on a worker: the caller (the render
// thread) never waits for the server or the fetcher's setup lock. The Licence
// screen shows a loading line and asks again until this prefix is gone.
inline constexpr char kLicenceLoadingPrefix[] = "Licence loading: ";

// Bytes Delete frees for a pinned backend: the source checkpoint plus an
// estimate of the converted GGUF. 0 for an unknown backend.
uint64_t model_installed_bytes(const std::string& backend);

// "3.9 GB": model_installed_bytes() rounded up to a tenth, for the delete
// confirmation.
std::string model_installed_text(uint64_t bytes);

// The pre-manager fixed-path GGUF for `backend` under `models_dir`:
// <dir>/<backend>.gguf first, then - for SHARP only - W18's two SHARP file
// names (sharp_2572gikvuh-f16-v1.gguf, then f32). Empty when no candidate is a
// regular file. `backend`'s own file is the only one another backend may see:
// a da2 job can never resolve SHARP weights. This is the one legacy lookup
// ModelService's "verified in place" check and the console's converter both use
// (platform/ps5/viewer/ps5_jobs.cpp).
Path legacy_model_path(const Path& models_dir, const std::string& backend);

// The GGUF a converter should load for `backend`: the manager's Ready record
// (the one truth, docs/ui-v1-plan.md section 2) when its file exists, else
// legacy_model_path(). Empty when the backend has no model file at all.
Path resolved_model_path(ModelManager& manager, const std::string& backend,
                         const Path& legacy_models_dir);

struct ModelServiceOptions {
    // Where licenses.json, models/ and downloads/ live. Empty: the manager's
    // state_dir(). ui.json is written here too.
    Path state_dir;
    // A verified `<dir>/<backend>.gguf` left by the old fixed-path layout counts
    // as Ready (no copy). Empty (the default) disables the check, so tests and a
    // build without the legacy folder do not depend on it.
    Path legacy_models_dir;
    // Free bytes at state_dir. Injectable for tests; the default asks the
    // filesystem and never refuses when it cannot answer.
    std::function<uint64_t(const Path&)> free_bytes;
    // Seconds on a monotonic clock, for the download's time estimate. Injectable
    // for tests; the default is std::chrono::steady_clock.
    std::function<double()> clock;
    // The rows list() serves are a snapshot: the render thread never reads the
    // state directory (issue-1-crash sharp-slow-report.md §3.3). The worker side
    // refreshes it on model events and, at most this often, when tick() says a
    // screen is up. 0 disables the tick-driven refresh; a huge value is the
    // "never rebuilt behind my back" test seam.
    double cache_refresh_seconds = 0.5;
    // One line per download event: started, a progress line about every ten
    // seconds (with ", no progress" when the bytes did not move; never while
    // verifying or converting), and the end with the manager's message. Empty
    // (the default) logs nothing. The console wires it to klog: without it a run
    // cannot tell "no bytes arrived" from "the panel did not repaint" - the
    // exact gap that hid the 0.6.1 stall (runs/issue-1-crash/sharp-dl-report.md).
    std::function<void(const std::string&)> log;
    // A transfer that moves no byte for this long is aborted (see
    // kTransferStallSeconds). A test seam: the local-server cases set it to a
    // second instead of waiting a minute. <= 0 disables the stall abort.
    double stall_timeout_seconds = kTransferStallSeconds;
};

class ModelService : public IModelService {
public:
    ModelService(ModelManager& manager, UiState& ui_state,
                 ModelServiceOptions options = ModelServiceOptions{});
    ~ModelService() override;

    ModelService(const ModelService&) = delete;
    ModelService& operator=(const ModelService&) = delete;

    std::vector<ModelView> list() const override;
    std::string licence_text(const std::string& id) override;
    Status accept(const std::string& id) override;
    Status download(const std::string& id) override;
    Status cancel(const std::string& id) override;
    Status remove(const std::string& id) override;
    Status select(const std::string& id) override;
    std::string selected() const override;

    // Marks the rows for a worker-side refresh at most four times a second. The
    // host calls this only while a screen that shows the rows is up, so an
    // external change (a legacy GGUF edited or deleted) still shows there and a
    // session with no such screen makes no /data call at all. No disk work on
    // this thread: list() keeps serving the old snapshot until the worker
    // publishes the new one.
    void tick(double dt) override;

    // True while the worker thread runs (download, verify or convert).
    bool downloading() const;
    std::string downloading_id() const;

    // The host's Quit bound. Signals the cancel, lets the stall watchdog repeat
    // the abort for at most kQuitWaitSeconds, and returns true only when the
    // download worker, the watchdog and a licence fetch have all finished (the
    // threads are then joined). False: a thread is still inside a fetcher call,
    // so this object, the manager and the fetcher must not be destroyed; the
    // host leaks the UI runtime instead, exactly as it already does for a
    // conversion (platform/ps5/viewer/vr_mode.cpp). Never enters an SDK call
    // itself: the abort is the watchdog's.
    bool stop();

    // What stop() left running, for the host's one line before it leaks the
    // runtime: "downloading sharp", "checking the SHA-256 of sharp",
    // "converting sharp", "fetching the licence text", "the stall watchdog".
    // Empty when nothing runs.
    std::string running_text() const;

    // Joins the worker, blocking until it is done. For tests and a host that
    // knows the transfer finishes; the Quit path uses stop() instead, which has
    // a bound.
    void wait_for_idle();

    const Path& state_dir() const { return state_dir_; }

private:
    void refresh_cache();
    std::vector<ModelView> build_views();
    // Publishes `views` under mutex_; the caller holds nothing.
    void publish_views(std::vector<ModelView> views);
    // Publishes `views` under mutex_; mutex_ must be held. Used for the final
    // snapshot, which clears the download overlay in the same critical section.
    void publish_locked(std::vector<ModelView> views);
    // The download worker's last act on the rows: build the final rows from the
    // record the manager saved, then clear the overlay and publish them at once.
    void publish_download_end();
    void join_finished_worker();
    void overlay_download_locked(std::vector<ModelView>& views) const;
    void download_loop(const std::string& id);
    void monitor_loop();
    void licence_loop(const std::string& id);
    void log_line(const std::string& text) const;
    bool active_locked(const std::string& id) const;
    Status start_download(const std::string& id);

    ModelManager& manager_;
    UiState& ui_state_;
    Path state_dir_;
    Path legacy_models_dir_;
    std::function<uint64_t(const Path&)> free_bytes_;
    std::function<double()> clock_;
    std::function<void(const std::string&)> log_;
    double stall_timeout_seconds_ = kTransferStallSeconds;
    double cache_refresh_seconds_ = 0.5;

    // Serialises the workers' rebuild-and-publish pairs. Without it a monitor
    // rebuild that started before the download worker saved its final record
    // could publish its older rows after the worker's, leaving "ready" hidden
    // until the next tick. Every rebuild reads the files at build time, so the
    // last one to run is always the freshest.
    std::mutex refresh_mutex_;
    // mutex_ guards the snapshot and the download state below. No disk work runs
    // under it: build_views() reads the files outside the lock, and only
    // publish_views() swaps the finished rows in under it.
    mutable std::mutex mutex_;
    mutable std::vector<ModelView> cache_;
    bool cache_stale_ = true;  // guarded by mutex_
    double tick_seconds_ = 0.0;  // guarded by mutex_
    std::chrono::steady_clock::time_point last_refresh_{};
    std::map<std::string, std::string> licence_texts_;

    std::atomic<bool> cancel_requested_{false};
    bool download_running_ = false;
    std::string download_backend_;
    float download_fraction_ = 0.0f;
    std::string download_stage_;
    uint64_t download_bytes_ = 0;  // received so far, from the byte stage of the progress
    uint64_t download_total_ = 0;
    DownloadRate download_rate_;
    // The log line's cadence: the clock reading of the next progress line, the
    // byte count it named, and whether a progress line has been written at all -
    // the first one is the transfer's start, where "no progress" would be a
    // lie (all guarded by mutex_).
    double log_next_seconds_ = 0.0;
    uint64_t log_bytes_ = 0;
    bool log_has_line_ = false;

    // The stall watchdog: it aborts a transfer whose byte count has not moved
    // for stall_timeout_seconds_, even while the transfer thread is blocked
    // inside a fetcher call. stall_requested_ is also part of the worker's
    // `cancelled` predicate, so a fetcher that polls it (libcurl on the desktop)
    // stops the same way; a blocked sceHttp read is reached through
    // ModelManager::abort_transfer -> IHttpFetcher::abort. The abort sets the
    // request's reason before this flag, so a chunk that arrives late reports
    // the stall, not a cancel.
    std::atomic<bool> stall_requested_{false};

    // The licence fetch: licence_text() only reads this state and starts a
    // worker, so the render thread never waits for the network or for the
    // fetcher's setup lock (which a wedged download can hold). Guarded by
    // mutex_ with licence_error_/licence_retry_at_.
    bool licence_fetching_ = false;
    std::string licence_error_id_;
    std::string licence_error_;
    double licence_retry_at_ = 0.0;

    std::thread worker_;
    std::thread monitor_;
    std::thread licence_worker_;
    std::condition_variable worker_done_cv_;
    std::condition_variable monitor_wake_;
    std::condition_variable monitor_done_cv_;
    std::condition_variable licence_done_cv_;
    // The threads' last act before returning (guarded by mutex_), so stop() can
    // wait with a bound and only then join them.
    bool worker_done_ = true;    // guarded by mutex_
    bool monitor_stop_ = false;  // guarded by mutex_
    bool monitor_done_ = false;  // guarded by mutex_
};

}  // namespace splat
