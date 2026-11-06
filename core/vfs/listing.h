// AsyncListing: a directory listing a screen asks for once per frame and reads
// when it is ready.
//
// A local directory is listed inside request(), so a console folder screen is
// ready on the same call it always was and nothing flickers. A location path
// (core/vfs/io.h) is listed by one worker thread; the UI thread keeps drawing,
// tick() publishes the finished snapshot without ever blocking, and busy() is
// what the screen shows "Loading..." for.
//
// One request at a time: a newer request supersedes a running one and its
// result is dropped (the generation check), so a fast d-pad cannot publish an
// old folder over a new one. `payload` carries whatever a screen built from the
// entries - its own item list - type-erased; it is published together with the
// entries, under the same lock, so a screen reads it only after ready().
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "vfs/io.h"

namespace splat::vfs {

class AsyncListing {
public:
    // What one listing publishes. A caller that needs more than the entries
    // returns its own structure through `payload`.
    struct Snapshot {
        std::vector<DirEntry> entries;
        std::shared_ptr<void> payload;
    };
    using Job = std::function<Result<Snapshot>()>;

    AsyncListing();
    ~AsyncListing();

    AsyncListing(const AsyncListing&) = delete;
    AsyncListing& operator=(const AsyncListing&) = delete;

    // One directory level through vfs::list.
    void request(const Path& directory);
    // The caller's own listing (album stats, a depth-limited photo walk, a
    // scene list with meta). Runs inline for a local directory and on the
    // worker for a location path.
    void request(const Path& directory, Job job);

    // True while a location listing is with the worker.
    bool busy() const;
    // A fresh result is published; cleared by the next request.
    bool ready() const;
    const std::vector<DirEntry>& entries() const;
    const std::shared_ptr<void>& payload() const;
    const std::string& error() const;
    const Path& directory() const;

    // Publishes a finished worker result. Never blocks: it only takes the
    // short state lock.
    void tick();

    // Drops a running request's result and clears the state (leaving a screen).
    void cancel();

private:
    static Result<Snapshot> list_one_level(const Path& directory);
    void worker_main();
    // Publishes the result when it belongs to the current request; false when
    // it was superseded and dropped.
    bool publish_locked(uint64_t generation, Result<Snapshot> result);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;

    Path directory_;
    bool busy_ = false;
    bool ready_ = false;
    std::vector<DirEntry> entries_;
    std::shared_ptr<void> payload_;
    std::string error_;
    uint64_t generation_ = 0;

    // The job the worker is running, and its answer waiting for tick().
    // The job carries the generation it was requested under: a finished job is
    // stamped with that, never with the current one, so a listing started for
    // folder A and answered after the screen asked for B is dropped instead of
    // published under B.
    Job pending_job_;
    bool has_pending_ = false;
    uint64_t pending_generation_ = 0;
    bool has_result_ = false;
    uint64_t result_generation_ = 0;
    Result<Snapshot> result_;

    std::thread worker_;
    bool stop_ = false;
};

}  // namespace splat::vfs
