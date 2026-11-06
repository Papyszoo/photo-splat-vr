// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/listing.h"

#include <utility>

namespace splat::vfs {

AsyncListing::AsyncListing() : worker_(&AsyncListing::worker_main, this) {}

AsyncListing::~AsyncListing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        has_pending_ = false;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Result<AsyncListing::Snapshot> AsyncListing::list_one_level(const Path& directory) {
    Snapshot snapshot;
    const Status listed = list(directory, &snapshot.entries);
    if (!listed.ok()) return Result<Snapshot>::failure(listed.message());
    return Result<Snapshot>::success(std::move(snapshot));
}

void AsyncListing::request(const Path& directory) {
    request(directory, [directory] { return list_one_level(directory); });
}

void AsyncListing::request(const Path& directory, Job job) {
    Job work = job ? std::move(job) : Job([directory] { return list_one_level(directory); });
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        directory_ = directory;
        entries_.clear();
        payload_.reset();
        error_.clear();
        ready_ = false;
        busy_ = false;
        generation = ++generation_;
        // A result the worker has not published yet belongs to the old
        // request; drop it rather than let tick() show it.
        has_result_ = false;
    }

    if (!is_remote(directory)) {
        // Local: the screen is ready when request() returns, exactly as the
        // direct gallery calls were. Still through the generation check, so a
        // concurrent remote job cannot publish during this call.
        Result<Snapshot> result = work();
        std::lock_guard<std::mutex> lock(mutex_);
        publish_locked(generation, std::move(result));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_job_ = std::move(work);
        pending_generation_ = generation;
        has_pending_ = true;
        busy_ = true;
    }
    wake_.notify_one();
}

void AsyncListing::worker_main() {
    for (;;) {
        Job job;
        uint64_t job_generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || has_pending_; });
            if (stop_) return;
            job = std::move(pending_job_);
            job_generation = pending_generation_;
            pending_job_ = nullptr;
            has_pending_ = false;
        }
        Result<Snapshot> result = job ? job() : Result<Snapshot>::failure("no listing job");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            result_ = std::move(result);
            result_generation_ = job_generation;
            has_result_ = true;
        }
        done_.notify_all();
    }
}

bool AsyncListing::publish_locked(uint64_t generation, Result<Snapshot> result) {
    if (generation != generation_) return false;  // superseded
    if (!result.ok()) {
        error_ = result.message();
        entries_.clear();
        payload_.reset();
        ready_ = true;
        return true;
    }
    entries_ = std::move(result.value().entries);
    payload_ = std::move(result.value().payload);
    error_.clear();
    ready_ = true;
    return true;
}

void AsyncListing::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_result_) return;
    has_result_ = false;
    Result<Snapshot> result = std::move(result_);
    result_ = Result<Snapshot>();
    // A dropped result says nothing about the request now running: busy() stays
    // true and the screen keeps drawing "Loading..." until the live one lands.
    if (publish_locked(result_generation_, std::move(result))) busy_ = false;
}

bool AsyncListing::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

bool AsyncListing::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

const std::vector<DirEntry>& AsyncListing::entries() const { return entries_; }

const std::shared_ptr<void>& AsyncListing::payload() const { return payload_; }

const std::string& AsyncListing::error() const { return error_; }

const Path& AsyncListing::directory() const { return directory_; }

void AsyncListing::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    has_result_ = false;
    pending_job_ = nullptr;
    has_pending_ = false;
    busy_ = false;
    ready_ = false;
    entries_.clear();
    payload_.reset();
    error_.clear();
}

}  // namespace splat::vfs
