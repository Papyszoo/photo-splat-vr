// The cross-thread handshake between a transfer and abort(), shared by the
// console fetcher (ps5_http_fetcher.cpp) and the host test.
//
// The console needs sceHttpAbortRequest on a request another thread is blocked
// in. The id must never be used after the request is deleted, and a delete must
// never run while an abort is about to use that id. This header keeps both rules
// without ever holding one global lock across an SDK call:
//
//  - the registry mutex guards only the list of live entries; it is held for a
//    vector copy, never while sceHttp is called;
//  - each entry has its own mutex, held across the one SDK call that uses the
//    entry's id (the owner's delete, or an abort);
//  - abort() takes that entry mutex with try_lock, so an entry whose delete (or
//    an earlier abort) has not returned can never block Cancel, the stall
//    watchdog or Quit.
//
// A skipped entry is either being torn down (the transfer does not need the
// abort) or already has an abort in flight; the caller retries on its next tick.
//
// Host-testable: no SDK is referenced here, so tests/live_requests_test.cpp
// drives the same policy with callbacks that block.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace splat::ps5 {

// One request id another thread may abort, and the outcome it reports.
struct LiveRequest {
    // Held by the owner while it publishes or clears+deletes the id, and by
    // abort() while it calls sceHttpAbortRequest: the id is valid for exactly as
    // long as its holder keeps this mutex.
    std::mutex mutex;
    int id = -1;  // guarded by mutex; -1: no request to abort
    std::atomic<int> reason{0};  // 0: no abort; else int(FetchAbort) + 1
};

// One request the abort call actually reached.
struct AbortAttempt {
    int id = -1;
    int code = 0;  // the SDK's return value
    // This entry had already been aborted once (whatever the reason): the
    // caller logs one line per transfer, not one per 250 ms tick.
    bool repeated = false;
};

class LiveRegistry {
public:
    LiveRegistry() = default;
    LiveRegistry(const LiveRegistry&) = delete;
    LiveRegistry& operator=(const LiveRegistry&) = delete;

    // Lists one request for the duration of a call. The owner keeps the entry
    // alive after it is unlisted: abort() may still hold a copy.
    class Scope {
    public:
        explicit Scope(LiveRegistry& registry) : registry_(registry) {
            std::lock_guard<std::mutex> lock(registry_.mutex_);
            registry_.live_.push_back(request_);
        }
        ~Scope() {
            std::lock_guard<std::mutex> lock(registry_.mutex_);
            for (auto it = registry_.live_.begin(); it != registry_.live_.end(); ++it) {
                if (*it == request_) {
                    registry_.live_.erase(it);
                    break;
                }
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        LiveRequest& request() { return *request_; }
        const LiveRequest& request() const { return *request_; }

    private:
        LiveRegistry& registry_;
        std::shared_ptr<LiveRequest> request_ = std::make_shared<LiveRequest>();
    };

    // Owner, after the request exists: publish the id abort() may use. Locks the
    // entry only, never the registry list.
    static void publish(LiveRequest& request, int id) {
        std::lock_guard<std::mutex> lock(request.mutex);
        request.id = id;
    }

    // Owner teardown: clear the id, then run `delete_request` (the SDK delete)
    // under the entry lock. An abort that already holds the lock finishes on the
    // still-live id; one that comes later sees -1, so a reused id can never be
    // aborted through this entry.
    template <class DeleteFn>
    static void retire(LiveRequest& request, DeleteFn&& delete_request) {
        std::lock_guard<std::mutex> lock(request.mutex);
        request.id = -1;
        delete_request();
    }

    // Aborts every listed request without waiting for any entry. `abort_request`
    // is the SDK call; it runs under that entry's lock, so the id stays valid.
    template <class AbortFn>
    std::vector<AbortAttempt> abort_all(int code, AbortFn&& abort_request) {
        std::vector<std::shared_ptr<LiveRequest>> entries;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries = live_;
        }
        std::vector<AbortAttempt> attempts;
        for (const std::shared_ptr<LiveRequest>& entry : entries) {
            std::unique_lock<std::mutex> guard(entry->mutex, std::try_to_lock);
            if (!guard.owns_lock()) continue;  // being deleted, or an abort is in flight
            const int id = entry->id;
            if (id < 0) continue;
            const int previous = entry->reason.load(std::memory_order_relaxed);
            entry->reason.store(code, std::memory_order_relaxed);
            const int rc = abort_request(id);
            attempts.push_back(AbortAttempt{id, rc, previous != 0});
        }
        return attempts;
    }

private:
    std::mutex mutex_;  // guards live_ only; never held across an SDK call
    std::vector<std::shared_ptr<LiveRequest>> live_;
};

}  // namespace splat::ps5
