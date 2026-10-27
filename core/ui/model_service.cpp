// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/model_service.h"

#include <algorithm>
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
      state_dir_(options.state_dir.empty() ? manager.state_dir() : options.state_dir),
      legacy_models_dir_(std::move(options.legacy_models_dir)),
      free_bytes_(std::move(options.free_bytes)) {
    if (!free_bytes_) free_bytes_ = default_free_bytes;
}

ModelService::~ModelService() {
    cancel_requested_.store(true);
    if (worker_.joinable()) worker_.join();
}

void ModelService::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_valid_ = false;
}

void ModelService::join_finished_worker() {
    if (worker_.joinable()) worker_.join();
}

bool ModelService::active_locked(const std::string& id) const {
    return download_running_ && download_backend_ == id;
}

void ModelService::rebuild_cache_locked() const {
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
        view.selected = ui_state_.selected_model == view.id;

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
    cache_ = std::move(views);
}

void ModelService::overlay_download_locked(std::vector<ModelView>& views) const {
    for (ModelView& view : views) {
        if (!active_locked(view.id)) continue;
        view.fraction = download_fraction_;
        view.message = download_stage_;
        if (download_stage_.find("verif") != std::string::npos) {
            view.state = "verifying";
        } else if (download_fraction_ >= 0.70f) {
            view.state = "converting";  // the manager reports 0.70 "verifying" first
        } else {
            view.state = "downloading";
        }
    }
}

std::vector<ModelView> ModelService::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cache_valid_) {
        rebuild_cache_locked();
        cache_valid_ = true;
    }
    std::vector<ModelView> views = cache_;
    overlay_download_locked(views);
    return views;
}

void ModelService::tick(double dt) {
    if (dt <= 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    refresh_seconds_ += dt;
    if (refresh_seconds_ < 0.25) return;
    refresh_seconds_ = 0.0;
    cache_valid_ = false;
}

std::string ModelService::licence_text(const std::string& id) {
    const ModelBackendSpec* spec = manager_.backend(id);
    if (spec == nullptr) return std::string(kLicenceUnavailablePrefix) + "no such model: " + id;
    if (spec->license_url.empty()) {
        return std::string(kLicenceUnavailablePrefix) + spec->display_name +
               " needs no licence acceptance (" + spec->license_name + ")";
    }

    const Result<std::string> text = manager_.fetch_license_text(id);
    if (!text.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        licence_texts_.erase(id);
        return std::string(kLicenceUnavailablePrefix) + text.message();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        licence_texts_[id] = text.value();
    }
    return text.value();
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
    invalidate();
    return accepted;
}

Status ModelService::start_download(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_requested_.store(false);
        download_backend_ = id;
        download_fraction_ = 0.0f;
        download_stage_ = "starting";
        download_running_ = true;
        cache_valid_ = false;
    }
    worker_ = std::thread([this, id] {
        Progress progress([this](float fraction, const std::string& stage) {
            std::lock_guard<std::mutex> lock(mutex_);
            download_fraction_ = std::max(download_fraction_, fraction);
            if (!stage.empty()) download_stage_ = stage;
        });
        const Status status = manager_.fetch(id, progress, [this] {
            return cancel_requested_.load(std::memory_order_relaxed);
        });
        std::lock_guard<std::mutex> lock(mutex_);
        download_running_ = false;
        download_backend_.clear();
        (void)status;  // the manager persists the failure in the record
        cache_valid_ = false;
    });
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

    const uint64_t required =
        spec->source_bytes + converted_bytes(id) + kFreeSpaceMarginBytes;
    const uint64_t available = free_bytes_(state_dir_);
    if (available != UINT64_MAX && available < required) {
        return Status::error("Downloading " + name + " needs ~" + required_space_text(required) +
                             " free; " + required_space_text(available) + " is available");
    }

    return start_download(id);
}

Status ModelService::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_locked(id)) {
        const ModelBackendSpec* spec = manager_.backend(id);
        const UiMeta* meta = spec != nullptr ? meta_for(id) : nullptr;
        const std::string name = meta != nullptr ? meta->name : id;
        return Status::error(name + " is not downloading");
    }
    // The fetcher polls this through the manager; the transfer stops at the next
    // chunk and the .part stays for a later resume.
    cancel_requested_.store(true);
    download_stage_ = "cancelling";
    cache_valid_ = false;
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
    invalidate();
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
        cache_valid_ = false;
    }
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
