// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/model_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include "model/checkpoint.h"
#include "model/da2_profile.h"

namespace splat {
namespace {

// Estimate of the converted GGUF on disk. Only the free-space check and the
// "Delete ... GB" text use it, so a rounded f16 size is enough (the real file is
// written by the converter; SHARP's is about 1.22 GiB, DA2-Small's about 47 MiB).
constexpr uint64_t kSharpConvertedBytes = 1310000000ull;
constexpr uint64_t kDa2ConvertedBytes = 49749088ull;
constexpr uint64_t kFreeSpaceMarginBytes = 256ull * 1024ull * 1024ull;
// How long stop() waits for the worker, the watchdog and a licence fetch. A
// transfer that honours abort() returns in well under a second; after this the
// host leaves the whole runtime to the process exit (the console's Quit path).
constexpr double kQuitWaitSeconds = 2.0;
// The watchdog's wake-up: 250 ms so a stall is aborted within a quarter second
// of the deadline and a cancel is noticed promptly. One wake-up per quarter
// second costs nothing next to the frame loop.
constexpr std::chrono::milliseconds kMonitorTick{250};
// A licence fetch that failed is not retried on every frame: a new attempt is
// made when the screen asks again after this long (opening it again is the
// normal case).
constexpr double kLicenceRetrySeconds = 10.0;

struct UiMeta {
    const char* id;
    const char* name;
    const char* blurb;
    const char* licence;
};

// The two pinned entries, in list order. Display text only; sizes, hashes and
// licences stay in the manager's specs.
constexpr UiMeta kUiMeta[] = {
    {"sharp", "SHARP", "Recommended, best quality", "Apple research"},
    {"da2", "DA2-Small", "Fast, approximate scene size", "Apache-2.0"},
};

const UiMeta* meta_for(const std::string& backend) {
    for (const UiMeta& meta : kUiMeta) {
        if (backend == meta.id) return &meta;
    }
    return nullptr;
}

uint64_t converted_bytes(const std::string& backend) {
    if (backend == "sharp") return kSharpConvertedBytes;
    if (backend == "da2") return kDa2ConvertedBytes;
    return 0;
}

uint64_t default_free_bytes(const Path& directory) {
    std::error_code error;
    Path probe = directory;
    while (!probe.empty() && !std::filesystem::exists(probe, error)) probe = probe.parent_path();
    if (probe.empty()) probe = ".";
    const std::filesystem::space_info info = std::filesystem::space(probe, error);
    if (error) return UINT64_MAX;  // cannot tell: never refuse for that reason
    return info.available;
}

// "4 GB" for the free-space refusal; "0.4 GB" below one.
std::string required_space_text(uint64_t bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char text[32];
    if (gib >= 1.0) {
        std::snprintf(text, sizeof(text), "%.0f GB", gib);
    } else {
        std::snprintf(text, sizeof(text), "%.1f GB", gib);
    }
    return text;
}

}  // namespace

uint64_t model_installed_bytes(const std::string& backend) {
    if (backend == "sharp") return kSharpCheckpointBytes + kSharpConvertedBytes;
    if (backend == "da2") return kDepthAnythingV2SmallBytes + kDa2ConvertedBytes;
    return 0;
}

std::string model_installed_text(uint64_t bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f GB", std::ceil(gib * 10.0) / 10.0);
    return text;
}

Path legacy_model_path(const Path& models_dir, const std::string& backend) {
    if (models_dir.empty() || backend.empty()) return {};
    std::error_code error;
    const auto regular_file = [&error](const Path& path) {
        error.clear();
        return std::filesystem::is_regular_file(path, error) && !error;
    };

    const Path own = models_dir / (backend + ".gguf");
    if (regular_file(own)) return own;

    // W18's fixed names for SHARP. Only SHARP: this is what used to hand a da2
    // job the resident SHARP weights when da2.gguf was absent.
    if (backend == "sharp") {
        const Path candidates[] = {
            models_dir / "sharp_2572gikvuh-f16-v1.gguf",
            models_dir / "sharp_2572gikvuh-f32-v1.gguf",
        };
        for (const Path& candidate : candidates) {
            if (regular_file(candidate)) return candidate;
        }
    }
    return {};
}

Path resolved_model_path(ModelManager& manager, const std::string& backend,
                         const Path& legacy_models_dir) {
    const Result<ModelRecord> record = manager.status(backend);
    if (record.ok() && record.value().state == ModelState::Ready) {
        const Path& path = record.value().gguf_path;
        std::error_code error;
        if (!path.empty() && std::filesystem::is_regular_file(path, error) && !error) {
            return path;
        }
    }
    return legacy_model_path(legacy_models_dir, backend);
}

ModelService::ModelService(ModelManager& manager, UiState& ui_state, ModelServiceOptions options)
    : manager_(manager),
      ui_state_(ui_state),
      state_dir_(options.state_dir.empty() ? manager.state_dir() : std::move(options.state_dir)),
      legacy_models_dir_(std::move(options.legacy_models_dir)),
      free_bytes_(std::move(options.free_bytes)),
      clock_(std::move(options.clock)),
      log_(std::move(options.log)),
      stall_timeout_seconds_(options.stall_timeout_seconds),
      cache_refresh_seconds_(options.cache_refresh_seconds) {
    if (!free_bytes_) free_bytes_ = default_free_bytes;
    if (!clock_) {
        clock_ = [] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
    }
    // The first snapshot is built here, before the first frame: from then on
    // list() only copies it and the workers replace it.
    refresh_cache();
    monitor_ = std::thread([this] { monitor_loop(); });
}

ModelService::~ModelService() {
    if (stop()) return;
    // A host reached Quit without the host's leak policy: a thread is still
    // inside a fetcher call and uses this object, the manager and the fetcher.
    // Nothing may be joined (a wedged SDK call would hold Quit) or freed under
    // it, so the threads are detached here; the console's Quit paths prevent
    // this by calling stop() first and leaking the runtime when it is false.
    std::string what = running_text();
    if (what.empty()) what = "a model thread";
    log_line("model download: " + what +
             " is still running 2 s after the stop; detaching it (the host must leak the panel)");
    if (worker_.joinable()) worker_.detach();
    if (monitor_.joinable()) monitor_.detach();
    if (licence_worker_.joinable()) licence_worker_.detach();
}

bool ModelService::stop() {
    // Signal only. The watchdog is the thread that calls the fetcher's abort;
    // this thread never enters an SDK call that could block here.
    cancel_requested_.store(true, std::memory_order_relaxed);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(kQuitWaitSeconds));

    bool worker_finished = true;
    if (worker_.joinable()) {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_finished =
            worker_done_cv_.wait_until(lock, deadline, [this] { return worker_done_; });
    }

    // Stop the watchdog only now: it kept repeating the abort for the whole
    // worker wait, which is what reaches a blocked sceHttp read.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitor_stop_ = true;
    }
    monitor_wake_.notify_all();
    bool monitor_finished = true;
    if (monitor_.joinable()) {
        std::unique_lock<std::mutex> lock(mutex_);
        monitor_finished =
            monitor_done_cv_.wait_until(lock, deadline, [this] { return monitor_done_; });
    }

    // A licence text fetch uses the same manager and fetcher, so it has to be
    // finished before anything may be destroyed too.
    bool licence_finished = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        licence_finished = !licence_fetching_;
    }
    if (licence_worker_.joinable()) {
        if (!licence_finished) {
            std::unique_lock<std::mutex> lock(mutex_);
            licence_finished =
                licence_done_cv_.wait_until(lock, deadline, [this] { return !licence_fetching_; });
        }
        if (licence_finished) licence_worker_.join();
    }

    // Each join here only runs for a thread whose done flag was seen; its last
    // act was that store, so the join is a thread exit and not a wait on the SDK.
    if (worker_finished && worker_.joinable()) worker_.join();
    if (monitor_finished && monitor_.joinable()) monitor_.join();

    return worker_finished && monitor_finished && licence_finished;
}

std::string ModelService::running_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    if (download_running_) {
        if (download_stage_.find("convert") != std::string::npos) {
            text = "converting " + download_backend_;
        } else if (download_stage_.find("verif") != std::string::npos) {
            text = "checking the SHA-256 of " + download_backend_;
        } else {
            text = "downloading " + download_backend_;
        }
    }
    if (licence_fetching_) {
        if (!text.empty()) text += " and ";
        text += "fetching the licence text";
    }
    if (!monitor_done_) {
        if (!text.empty()) text += " and ";
        text += "the stall watchdog";
    }
    return text;
}

void ModelService::join_finished_worker() {
    if (worker_.joinable()) worker_.join();
}

void ModelService::log_line(const std::string& text) const {
    if (log_) log_(text);
}

bool ModelService::active_locked(const std::string& id) const {
    return download_running_ && download_backend_ == id;
}

std::vector<ModelView> ModelService::build_views() {
    // The selection is the one piece of UiState this needs; copy it under the
    // lock so the rest runs without holding anything the render thread wants.
    std::string selected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selected = ui_state_.selected_model;
    }

    std::vector<ModelView> views;
    for (const UiMeta& meta : kUiMeta) {
        const ModelBackendSpec* spec = manager_.backend(meta.id);
        if (spec == nullptr) continue;

        ModelView view;
        view.id = spec->name;
        view.name = meta.name;
        view.blurb = meta.blurb;
        view.size_bytes = spec->source_bytes;
        view.licence_name =
            spec->license_url.empty() ? std::string(spec->license_name) : meta.licence;
        if (view.licence_name.empty()) view.licence_name = spec->license_name;
        view.gated = !spec->license_url.empty();
        view.offered = true;
        view.selected = selected == view.id;

        const Result<ModelRecord> record = manager_.status(spec->name);
        if (!record.ok()) {
            view.state = "failed";
            view.message = record.message();
        } else {
            view.state = model_state_name(record.value().state);
            view.message = record.value().message;
            if (record.value().state == ModelState::Ready) {
                view.fraction = 1.0f;
            } else if ((record.value().state == ModelState::Accepted ||
                        record.value().state == ModelState::Downloading ||
                        record.value().state == ModelState::Failed) &&
                       record.value().downloaded_bytes > 0 && spec->source_bytes > 0) {
                // A paused or cancelled download shows the part it already has.
                const double fraction = static_cast<double>(record.value().downloaded_bytes) /
                                        static_cast<double>(spec->source_bytes);
                view.fraction = static_cast<float>(std::min(1.0, fraction));
            }
        }

        // The old fixed-path layout, verified in place, counts as Ready without
        // a copy. The lookup is shared with the console's converter
        // (legacy_model_path), so both see the same file, including W18's SHARP
        // names - and a non-SHARP backend never sees them.
        if (view.state != "ready" && !legacy_models_dir_.empty()) {
            const Path legacy = legacy_model_path(legacy_models_dir_, spec->name);
            if (!legacy.empty() && manager_.verify_gguf(spec->name, legacy).ok()) {
                view.state = "ready";
                view.fraction = 1.0f;
                view.message = "ready (models/" + legacy.filename().string() + ")";
            }
        }
        views.push_back(std::move(view));
    }
    return views;
}

void ModelService::publish_locked(std::vector<ModelView> views) {
    cache_ = std::move(views);
    cache_stale_ = false;
    last_refresh_ = std::chrono::steady_clock::now();
}

void ModelService::publish_views(std::vector<ModelView> views) {
    std::lock_guard<std::mutex> lock(mutex_);
    publish_locked(std::move(views));
}

void ModelService::publish_download_end() {
    // The final rows come from the record the manager just saved, so they are
    // built before the overlay is cleared. Clearing download_running_ first
    // would let list() serve the last snapshot - whose record still said
    // Downloading, which reads as "Paused" with a Download hint - for the whole
    // rebuild (issue-1-crash sharp-fast-verify.md section 4.3b). Both happen in
    // the one critical section, so list() sees either the overlay or the final
    // rows and never the gap between them.
    std::lock_guard<std::mutex> serialize(refresh_mutex_);
    std::vector<ModelView> views = build_views();
    std::lock_guard<std::mutex> lock(mutex_);
    download_running_ = false;
    download_backend_.clear();
    publish_locked(std::move(views));
}

void ModelService::refresh_cache() {
    // One rebuild at a time (see refresh_mutex_): the last publish is the
    // freshest, so a download's final state cannot be overwritten by a rebuild
    // that began before it.
    std::lock_guard<std::mutex> serialize(refresh_mutex_);
    publish_views(build_views());
}

void ModelService::overlay_download_locked(std::vector<ModelView>& views) const {
    if (!download_running_) return;
    for (ModelView& view : views) {
        if (view.id != download_backend_) continue;
        view.fraction = download_fraction_;
        view.message = download_stage_;
        if (download_stage_.find("verif") != std::string::npos) {
            view.state = "verifying";
        } else if (download_fraction_ >= 0.70f) {
            view.state = "converting";  // the manager reports 0.70 "verifying" first
        } else {
            view.state = "downloading";
            // Verify and convert keep their stage text: only the transfer has bytes.
            view.eta_seconds = download_rate_.seconds_left(
                download_total_ > download_bytes_ ? download_total_ - download_bytes_ : 0);
        }
    }
}

std::vector<ModelView> ModelService::list() const {
    // A copy of the last published rows plus the live download fields. No file
    // is opened, stat'ed or parsed here: this runs on the render thread every
    // frame (issue-1-crash sharp-slow-report.md §3.3).
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ModelView> views = cache_;
    overlay_download_locked(views);
    return views;
}

void ModelService::tick(double dt) {
    if (dt <= 0.0) return;
    // A screen that shows the rows is up (the host calls this only then): ask
    // the worker side to look at the state directory again, at most four times
    // a second, for a change made outside the app (a legacy GGUF edited or
    // deleted). This only sets a flag; the read is the stall watch's job, so a
    // /data stall never delays a frame. With no such screen up nothing calls
    // this and the worker never touches /data (issue-1-crash
    // sharp-fast-verify.md section 4.3a).
    std::lock_guard<std::mutex> lock(mutex_);
    tick_seconds_ += dt;
    if (tick_seconds_ < 0.25) return;
    tick_seconds_ = 0.0;
    cache_stale_ = true;
}

void ModelService::download_loop(const std::string& id) {
    Progress progress([this, &id](float fraction, const std::string& stage) {
        std::string report;  // built under the lock, logged outside it
        bool stage_changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            download_fraction_ = std::max(download_fraction_, fraction);
            if (!stage.empty() && stage != download_stage_) {
                // The row's state comes from the stage (downloading, verifying,
                // converting): a change is the moment to re-read the record,
                // on the worker that owns the disk.
                download_stage_ = stage;
                stage_changed = true;
            }
            // One clock reading per report: the rate's sample and the log's
            // cadence must not disagree (nor cost a second call).
            const double now = clock_();
            if (stage == "downloading" && download_total_ > 0) {
                // The bar is the whole pipeline (download 0.05 .. 0.70); the time
                // estimate wants the transfer's bytes.
                const double part = (fraction - kFetchDownloadStart) / kFetchDownloadSpan;
                download_bytes_ = static_cast<uint64_t>(
                    std::clamp(part, 0.0, 1.0) * static_cast<double>(download_total_));
                download_rate_.add(now, download_bytes_);
            }
            // About one line per ten seconds, so an unattended run's klog says
            // whether bytes arrived without drowning the log at 1000 reports a
            // second. A stalled transfer keeps reporting the same fraction:
            // that line is the one a wedged download is read from.
            if (log_next_seconds_ == 0.0) log_next_seconds_ = now + 10.0;
            if (now >= log_next_seconds_) {
                log_next_seconds_ = now + 10.0;
                const uint64_t moved =
                    download_bytes_ > log_bytes_ ? download_bytes_ - log_bytes_ : 0;
                log_bytes_ = download_bytes_;
                char text[160];
                std::snprintf(text, sizeof(text), "model download: %s %s %.1f%% (%llu of %llu bytes)",
                              id.c_str(), download_stage_.c_str(),
                              static_cast<double>(download_fraction_) * 100.0,
                              static_cast<unsigned long long>(download_bytes_),
                              static_cast<unsigned long long>(download_total_));
                report = text;
                // Only the transfer has bytes. Verify and convert report a
                // fraction too, with the file already complete: saying "no
                // progress" there reads as a stalled transfer (D3). The first
                // line is the start of the transfer itself, where zero bytes is
                // the expected state.
                if (moved == 0 && stage == "downloading" && log_has_line_) {
                    report += ", no progress";
                }
                log_has_line_ = true;
            }
        }
        if (stage_changed) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cache_stale_ = true;
            }
            monitor_wake_.notify_all();
        }
        if (!report.empty()) log_line(report);
    });
    const Status status = manager_.fetch(id, progress, [this] {
        // cancel_requested_: the user's Square or the title's shutdown.
        // stall_requested_: the watchdog; a fetcher that polls this (libcurl)
        // stops at its next progress callback, while a blocked sceHttp read is
        // reached through ModelManager::abort_transfer.
        return cancel_requested_.load(std::memory_order_relaxed) ||
               stall_requested_.load(std::memory_order_relaxed);
    });
    // The final state (ready, or the failure the manager persisted) is
    // published by this worker before it reports done: a wait_for_idle() caller
    // and the next list() see it without touching the disk, and list() never
    // sees the stale "paused" row in between (publish_download_end()).
    publish_download_end();
    // The manager persists the failure in the record; the line makes the
    // reason reachable from a klog too.
    log_line("model download: " + id +
             (status.ok() ? " finished" : " failed: " + status.message()));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_done_ = true;
    }
    worker_done_cv_.notify_all();
}

void ModelService::monitor_loop() {
    // The one disk cleanup of the worker side, before the first tick: a Ready
    // SHARP install reclaims a 0.6.1 .pt that the old download-then-convert path
    // left beside the GGUF, and a hard-killed streaming attempt's
    // models/sharp.gguf.part goes with it (nothing resumes that file).
    // Never on the render thread.
    manager_.reclaim_streamed_sources();
    uint64_t last_bytes = 0;
    auto last_change = std::chrono::steady_clock::now();
    for (;;) {
        bool stale = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            monitor_wake_.wait_for(lock, kMonitorTick, [this] { return monitor_stop_; });
            if (monitor_stop_) break;
            // The rows: only this thread (and the download worker) reads the
            // state directory, so the render thread's list() never waits on
            // /data. The spacing keeps a screen's 4/s tick at ~2 rebuilds/s.
            const double spacing = std::max(0.05, cache_refresh_seconds_);
            stale = cache_stale_ &&
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - last_refresh_)
                            .count() >= spacing;
        }
        if (stale) refresh_cache();

        if (!manager_.transfer_active()) {
            // Outside the network phase (before HEAD, and verify/convert).
            last_bytes = 0;
            last_change = std::chrono::steady_clock::now();
            continue;
        }
        if (cancel_requested_.load(std::memory_order_relaxed)) {
            // Cancel and Quit: abort until the transfer is gone. Repeated so a
            // cancel that arrives just before the request is registered still
            // reaches it; with no live request abort() is a no-op, and the
            // fetcher logs the line once per transfer.
            manager_.abort_transfer(FetchAbort::cancelled);
            last_change = std::chrono::steady_clock::now();
            continue;
        }
        const uint64_t bytes = manager_.transfer_bytes();
        const auto now = std::chrono::steady_clock::now();
        if (bytes != last_bytes) {
            last_bytes = bytes;
            last_change = now;
            continue;
        }
        if (stall_timeout_seconds_ <= 0.0) continue;
        const double still = std::chrono::duration<double>(now - last_change).count();
        if (still < stall_timeout_seconds_) continue;

        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = download_backend_;
        }
        if (id.empty()) id = "the model";
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.0f", still);
        log_line("model download: " + id + " moved no byte for " + seconds +
                 " s; aborting the transfer");
        // The request's reason must be set before the polled flag: a chunk that
        // arrives after this point must report the stall, not a cancel (the
        // wording race the verifier found).
        manager_.abort_transfer(FetchAbort::stalled);
        stall_requested_.store(true, std::memory_order_relaxed);
        // One attempt per deadline: if the abort did not unblock the call, try
        // again after another full deadline rather than every tick.
        last_change = now;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitor_done_ = true;
    }
    monitor_done_cv_.notify_all();
}

void ModelService::licence_loop(const std::string& id) {
    const Result<std::string> text = manager_.fetch_license_text(id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text.ok()) {
            licence_texts_[id] = text.value();
            licence_error_id_.clear();
            licence_error_.clear();
        } else {
            licence_error_id_ = id;
            licence_error_ = std::string(kLicenceUnavailablePrefix) + text.message();
            licence_retry_at_ = clock_() + kLicenceRetrySeconds;
        }
        licence_fetching_ = false;
    }
    licence_done_cv_.notify_all();
}

std::string ModelService::licence_text(const std::string& id) {
    const ModelBackendSpec* spec = manager_.backend(id);
    if (spec == nullptr) return std::string(kLicenceUnavailablePrefix) + "no such model: " + id;
    if (spec->license_url.empty()) {
        return std::string(kLicenceUnavailablePrefix) + spec->display_name +
               " needs no licence acceptance (" + spec->license_name + ")";
    }

    const std::string loading = std::string(kLicenceLoadingPrefix) +
                                (spec->display_name.empty() ? id : spec->display_name);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = licence_texts_.find(id);
        if (found != licence_texts_.end()) return found->second;
        if (licence_fetching_) return loading;
        if (licence_error_id_ == id && !licence_error_.empty()) {
            if (clock_() < licence_retry_at_) return licence_error_;
            licence_error_id_.clear();
            licence_error_.clear();
        }
        licence_fetching_ = true;
    }
    // The previous fetch has finished (licence_fetching_ was false), so this join
    // is a thread's exit and cannot wait for the network.
    if (licence_worker_.joinable()) licence_worker_.join();
    licence_worker_ = std::thread([this, id] { licence_loop(id); });
    return loading;
}

Status ModelService::accept(const std::string& id) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = licence_texts_.find(id);
        if (found != licence_texts_.end()) text = found->second;
    }
    if (text.empty()) {
        return Status::error(std::string(kLicenceUnavailablePrefix) +
                             "show the licence before accepting it");
    }
    const Status accepted = manager_.accept_license(id, text);
    refresh_cache();  // a one-off state change; the row must say "accepted" now
    return accepted;
}

Status ModelService::start_download(const std::string& id) {
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_requested_.store(false);
        stall_requested_.store(false);
        worker_done_ = false;
        download_backend_ = id;
        download_fraction_ = 0.0f;
        download_stage_ = "starting";
        download_bytes_ = 0;
        const ModelBackendSpec* spec = manager_.backend(id);
        download_total_ = spec != nullptr ? spec->source_bytes : 0;
        total = download_total_;
        download_rate_.reset();
        download_running_ = true;
        log_next_seconds_ = 0.0;
        log_bytes_ = 0;
        log_has_line_ = false;
        cache_stale_ = true;  // the row's own state changes; the overlay carries the progress
    }
    log_line("model download: " + id + " started (" + std::to_string(total) + " bytes)");
    worker_ = std::thread([this, id] { download_loop(id); });
    return Status::success();
}

Status ModelService::download(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (download_running_) {
            if (download_backend_ == id) return Status::success();
            const auto found = std::find_if(cache_.begin(), cache_.end(),
                                            [this](const ModelView& view) {
                                                return view.id == download_backend_;
                                            });
            const std::string other =
                found == cache_.end() ? download_backend_ : found->name;
            return Status::error(other + " is downloading; wait or cancel it first");
        }
    }
    join_finished_worker();

    const ModelBackendSpec* spec = manager_.backend(id);
    if (spec == nullptr) return Status::error("no such model: " + id);
    const UiMeta* meta = meta_for(id);
    const std::string name = meta != nullptr ? meta->name : spec->display_name;

    if (!spec->license_url.empty() && !manager_.license_accepted(id)) {
        return Status::error("accept the " + spec->license_name + " licence first");
    }

    // The streaming path never stores the checkpoint, so it needs only what it
    // writes: the GGUF plus the margin. The file path writes the .pt and then
    // the GGUF, so it needs both. Asking for 4.4 GB here would refuse the very
    // consoles the streaming path is for (2-4 GB free).
    const uint64_t source_bytes = manager_.streams_source(id) ? 0 : spec->source_bytes;
    const uint64_t required = source_bytes + converted_bytes(id) + kFreeSpaceMarginBytes;
    const uint64_t available = free_bytes_(state_dir_);
    if (available != UINT64_MAX && available < required) {
        return Status::error("Downloading " + name + " needs ~" + required_space_text(required) +
                             " free; " + required_space_text(available) + " is available");
    }

    return start_download(id);
}

Status ModelService::cancel(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_locked(id)) {
            const ModelBackendSpec* spec = manager_.backend(id);
            const UiMeta* meta = spec != nullptr ? meta_for(id) : nullptr;
            const std::string name = meta != nullptr ? meta->name : id;
            return Status::error(name + " is not downloading");
        }
        // The fetcher polls this through the manager; the transfer stops at the
        // next chunk and the .part stays for a later resume.
        cancel_requested_.store(true);
        download_stage_ = "cancelling";
        cache_stale_ = true;
    }
    // No abort on this thread: Cancel runs on the render thread, and
    // sceHttpAbortRequest can block. The watchdog's cancel branch (monitor_loop)
    // repeats the same abort within one tick, which is what reaches a read
    // blocked inside sceHttp.
    return Status::success();
}

Status ModelService::remove(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (download_running_) {
            // Name the download that is in the way and, when it differs, the
            // model the user asked to delete: deleting SHARP during the
            // DA2-Small transfer used to say "cancel the SHARP download".
            const UiMeta* busy_meta = meta_for(download_backend_);
            const std::string busy =
                busy_meta != nullptr ? busy_meta->name : download_backend_;
            if (download_backend_ == id) {
                return Status::error("cancel the " + busy + " download before deleting it");
            }
            const ModelBackendSpec* spec = manager_.backend(id);
            const UiMeta* meta = spec != nullptr ? meta_for(id) : nullptr;
            const std::string name = meta != nullptr ? meta->name : id;
            return Status::error("cancel the " + busy + " download before deleting " + name);
        }
    }
    join_finished_worker();

    const ModelBackendSpec* spec = manager_.backend(id);
    if (spec == nullptr) return Status::error("no such model: " + id);

    // The verified source, a paused .part, the converted GGUF, the record (which
    // carries the state) and the legacy GGUF. licenses.json is not touched: the
    // acceptance survives a delete.
    std::vector<Path> files;
    files.push_back(manager_.downloads_dir() / spec->source_filename);
    files.push_back(manager_.downloads_dir() / (spec->source_filename + ".part"));
    files.push_back(segments_path(files.back()));
    files.push_back(manager_.gguf_path(id));
    files.push_back(manager_.state_dir() / "models" / (id + ".json"));
    if (!legacy_models_dir_.empty()) files.push_back(legacy_models_dir_ / (id + ".gguf"));

    for (const Path& file : files) {
        std::error_code error;
        std::filesystem::remove(file, error);
        if (error) {
            return Status::error("cannot delete " + file.string() + ": " + error.message());
        }
    }

    bool clears_selection = false;
    UiState next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clears_selection = ui_state_.selected_model == id;
        next = ui_state_;
    }
    if (clears_selection) {
        next.selected_model.clear();
        const Status saved = save_ui_state(state_dir_, next);
        if (!saved.ok()) return Status::error("cannot save the selection: " + saved.message());
        std::lock_guard<std::mutex> lock(mutex_);
        ui_state_ = next;
    }
    refresh_cache();
    return Status::success();
}

Status ModelService::select(const std::string& id) {
    const std::vector<ModelView> views = list();
    const auto found = std::find_if(views.begin(), views.end(),
                                    [&id](const ModelView& view) { return view.id == id; });
    if (found == views.end()) return Status::error("no such model: " + id);
    if (found->state != "ready") return Status::error(found->name + " is not ready yet");

    UiState next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = ui_state_;
    }
    next.selected_model = id;
    const Status saved = save_ui_state(state_dir_, next);
    if (!saved.ok()) return Status::error("cannot save the selection: " + saved.message());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ui_state_ = next;
    }
    refresh_cache();  // the row's "selected" mark moved
    return Status::success();
}

std::string ModelService::selected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ui_state_.selected_model;
}

bool ModelService::downloading() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return download_running_;
}

std::string ModelService::downloading_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return download_backend_;
}

void ModelService::wait_for_idle() { join_finished_worker(); }

}  // namespace splat
