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
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "model/model_manager.h"
#include "ui/services.h"
#include "ui/ui_state.h"

namespace splat {

// A licence_text() result that is a message rather than a licence starts with
// this prefix; the Licence screen shows the rest and offers no acceptance.
inline constexpr char kLicenceUnavailablePrefix[] = "Licence unavailable: ";

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

    // Refreshes the cached rows from the manager at most four times a second and
    // keeps the live download progress current.
    void tick(double dt) override;

    // True while the worker thread runs (download, verify or convert).
    bool downloading() const;
    std::string downloading_id() const;

    // Joins the worker, blocking until it is done. For the host's shutdown and
    // for tests; the destructor does the same.
    void wait_for_idle();

    const Path& state_dir() const { return state_dir_; }

private:
    void invalidate();
    void join_finished_worker();
    void rebuild_cache_locked() const;
    void overlay_download_locked(std::vector<ModelView>& views) const;
    bool active_locked(const std::string& id) const;
    Status start_download(const std::string& id);

    ModelManager& manager_;
    UiState& ui_state_;
    Path state_dir_;
    Path legacy_models_dir_;
    std::function<uint64_t(const Path&)> free_bytes_;

    mutable std::mutex mutex_;
    mutable std::vector<ModelView> cache_;
    mutable bool cache_valid_ = false;
    mutable double refresh_seconds_ = 0.0;
    std::map<std::string, std::string> licence_texts_;

    std::atomic<bool> cancel_requested_{false};
    bool download_running_ = false;
    std::string download_backend_;
    float download_fraction_ = 0.0f;
    std::string download_stage_;
    std::thread worker_;
};

}  // namespace splat
