#include "render/scene_playlist.h"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace splat {

PlaylistEntry playlist_entry(const SceneEntry& scene) {
    return PlaylistEntry{scene.folder, scene.folder / "scene.psplat"};
}

std::vector<PlaylistEntry> playlist_entries(const std::vector<SceneEntry>& scenes) {
    std::vector<PlaylistEntry> entries;
    entries.reserve(scenes.size());
    for (const SceneEntry& scene : scenes) entries.push_back(playlist_entry(scene));
    return entries;
}

Result<ScenePlaylist> ScenePlaylist::from_folder(const Path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return Result<ScenePlaylist>::failure("not a directory: " + directory.string());
    }

    std::vector<PlaylistEntry> entries;
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        std::error_code item_error;
        if (!item.is_directory(item_error) || item_error) continue;
        const Path scene_file = item.path() / "scene.psplat";
        if (std::filesystem::exists(scene_file, item_error) && !item_error) {
            entries.push_back({item.path(), scene_file});
        }
    }
    if (error) {
        return Result<ScenePlaylist>::failure("cannot read " + directory.string() + ": " +
                                              error.message());
    }

    // A folder given directly is also a valid one-scene playlist.
    if (entries.empty()) {
        const Path scene_file = directory / "scene.psplat";
        std::error_code exists_error;
        if (std::filesystem::exists(scene_file, exists_error) && !exists_error) {
            entries.push_back({directory, scene_file});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const PlaylistEntry& a, const PlaylistEntry& b) {
        return a.folder.filename().string() < b.folder.filename().string();
    });

    if (entries.empty()) {
        return Result<ScenePlaylist>::failure("no scene folders under " + directory.string());
    }

    ScenePlaylist playlist;
    playlist.entries_ = std::move(entries);
    return Result<ScenePlaylist>::success(std::move(playlist));
}

Result<ScenePlaylist> ScenePlaylist::from_entries(std::vector<PlaylistEntry> entries) {
    if (entries.empty()) return Result<ScenePlaylist>::failure("no scene entries");
    ScenePlaylist playlist;
    playlist.entries_ = std::move(entries);
    return Result<ScenePlaylist>::success(std::move(playlist));
}

void ScenePlaylist::next() {
    if (entries_.empty()) return;
    index_ = (index_ + 1) % entries_.size();
}

void ScenePlaylist::previous() {
    if (entries_.empty()) return;
    index_ = (index_ + entries_.size() - 1) % entries_.size();
}

void ScenePlaylist::set_index(size_t index) {
    if (entries_.empty()) return;
    index_ = index % entries_.size();
}

ScenePreloader::~ScenePreloader() { join(); }

void ScenePreloader::join() {
    if (worker_.joinable()) worker_.join();
}

void ScenePreloader::start(const Path& scene_path) {
    join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::running;
        path_ = scene_path;
        loaded_ = LoadedScene{};
        error_.clear();
    }
    worker_ = std::thread([this, scene_path]() {
        const auto start = std::chrono::steady_clock::now();
        Result<LoadedScene> result = read_scene_path(scene_path);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        finish(std::move(result), ms);
    });
}

void ScenePreloader::finish(Result<LoadedScene> result, double milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_load_ms_ = milliseconds;
    if (result.ok()) {
        loaded_ = result.take();
        state_ = State::ready;
    } else {
        error_ = result.message();
        state_ = State::failed;
    }
}

bool ScenePreloader::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::running;
}

bool ScenePreloader::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::ready;
}

bool ScenePreloader::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::failed;
}

std::string ScenePreloader::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

Path ScenePreloader::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

Result<LoadedScene> ScenePreloader::take() {
    join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::ready) {
        return Result<LoadedScene>::failure(state_ == State::failed
                                                ? "scene preload failed: " + error_
                                                : "scene preload is not ready");
    }
    LoadedScene loaded = std::move(loaded_);
    loaded_ = LoadedScene{};
    state_ = State::idle;
    return Result<LoadedScene>::success(std::move(loaded));
}

void ScenePreloader::wait() {
    join();
    std::lock_guard<std::mutex> lock(mutex_);
    // The worker always leaves the state at ready or failed before the thread
    // ends, so this drops a finished result as well as a cancelled one. A
    // failed() left latched would make the viewer log and cancel every frame.
    state_ = State::idle;
    loaded_ = LoadedScene{};
    error_.clear();
}

double ScenePreloader::last_load_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_load_ms_;
}

}  // namespace splat
