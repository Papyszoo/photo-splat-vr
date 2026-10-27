#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "gallery/gallery.h"
#include "scene/scene_folder.h"

namespace splat {

struct PlaylistEntry {
    Path folder;      // folder holding scene.psplat and meta.json
    Path scene_file;  // <folder>/scene.psplat
};

// Entry for one gallery scene; the album's scene order is kept as given.
PlaylistEntry playlist_entry(const SceneEntry& scene);
std::vector<PlaylistEntry> playlist_entries(const std::vector<SceneEntry>& scenes);

// Ordered list of scene folders for L1/R1 previous/next. Opening a directory
// takes every immediate subdirectory that contains scene.psplat, sorted by
// folder name; a directory that itself holds scene.psplat is a one-entry
// playlist. next()/previous() wrap around.
class ScenePlaylist {
public:
    static Result<ScenePlaylist> from_folder(const Path& directory);

    // Entries in the given order; fails when the list is empty.
    static Result<ScenePlaylist> from_entries(std::vector<PlaylistEntry> entries);

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    size_t index() const { return index_; }
    const PlaylistEntry& current() const { return entries_[index_]; }
    const PlaylistEntry& entry(size_t index) const { return entries_[index]; }

    void next();
    void previous();
    void set_index(size_t index);

private:
    std::vector<PlaylistEntry> entries_;
    size_t index_ = 0;
};

// One-deep background scene loader. The worker thread reads and validates the
// file; the renderer uploads the result into its staging buffers on the main
// thread. While the worker runs (or its result waits), the currently displayed
// scene and its GPU buffers are untouched.
class ScenePreloader {
public:
    ScenePreloader() = default;
    ~ScenePreloader();
    ScenePreloader(const ScenePreloader&) = delete;
    ScenePreloader& operator=(const ScenePreloader&) = delete;

    // Joins any previous worker, then loads `scene_path` on a new thread.
    void start(const Path& scene_path);

    bool running() const;
    bool ready() const;
    bool failed() const;
    std::string error() const;
    // Copy, taken under the mutex: the worker's start() rewrites path_ on the
    // main thread and the main thread polls it every frame.
    Path path() const;

    // Moves the loaded scene out. Only valid when ready(); resets the slot.
    Result<LoadedScene> take();

    // Waits for the worker (if any) and drops its result, including a failure
    // and any loaded scene, so the preloader is idle again afterwards.
    void wait();
    void cancel() { wait(); }

    // Wall time of the last completed load, milliseconds.
    double last_load_ms() const;

private:
    void join();
    void finish(Result<LoadedScene> result, double milliseconds);

    mutable std::mutex mutex_;
    enum class State { idle, running, ready, failed };
    State state_ = State::idle;
    Path path_;
    LoadedScene loaded_;
    std::string error_;
    double last_load_ms_ = 0.0;
    std::thread worker_;
};

}  // namespace splat
