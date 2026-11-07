// The hand-off between the render loop and the TV worker (0.6.0): a few frame
// slots and a bounded drop queue. Header-only and pure, so the desktop test
// drives the exact code the console runs.
//
// The render loop must never wait for the TV. It asks for a free slot before a
// frame, and when none is free it simply does not produce a TV frame this time
// (the TV runs at 30 fps next to a 60 fps loop, so a late worker costs the TV a
// frame, never the headset). A finished slot is published; the worker always
// takes the NEWEST published one, and an older one it never got to is dropped
// back to free. So the queue is bounded by the slot count and the TV shows the
// latest picture, not a growing backlog.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace splat {

class TvSlotQueue {
public:
    explicit TvSlotQueue(int slots = 3)
        : state_(static_cast<size_t>(slots < 1 ? 1 : slots), State::Free) {}

    int slots() const { return static_cast<int>(state_.size()); }

    // Render thread. A free slot to write the next TV frame into (now Writing),
    // or -1 when every slot is busy: the frame is dropped, nothing waits.
    int acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return -1;
        for (size_t i = 0; i < state_.size(); ++i) {
            if (state_[i] == State::Free) {
                state_[i] = State::Writing;
                return static_cast<int>(i);
            }
        }
        ++refused_;
        return -1;
    }

    // Render thread. The slot is written: hand it to the worker. A frame the
    // worker has not taken yet is replaced (its slot goes back to free).
    void publish(int slot) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!valid(slot) || state_[static_cast<size_t>(slot)] != State::Writing) return;
            if (pending_ >= 0) {
                state_[static_cast<size_t>(pending_)] = State::Free;
                ++replaced_;
            }
            state_[static_cast<size_t>(slot)] = State::Pending;
            pending_ = slot;
        }
        wake_.notify_one();
    }

    // Render thread. The slot was acquired but no frame came out of it.
    void cancel(int slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (valid(slot) && state_[static_cast<size_t>(slot)] == State::Writing) {
            state_[static_cast<size_t>(slot)] = State::Free;
        }
    }

    // Worker. The newest published slot (now Reading), waiting up to `timeout`
    // for one; -1 on a timeout or after close().
    int take(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, timeout, [this] { return pending_ >= 0 || closed_; });
        if (pending_ < 0) return -1;
        const int slot = pending_;
        pending_ = -1;
        state_[static_cast<size_t>(slot)] = State::Reading;
        return slot;
    }

    // Worker. Done with the slot.
    void release(int slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (valid(slot) && state_[static_cast<size_t>(slot)] == State::Reading) {
            state_[static_cast<size_t>(slot)] = State::Free;
        }
    }

    // No new slots; wakes a waiting worker.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        wake_.notify_all();
    }

    // Frames the render loop could not start because every slot was busy, and
    // published frames the worker never got to before a newer one arrived.
    uint64_t refused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return refused_;
    }
    uint64_t replaced() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return replaced_;
    }

private:
    enum class State { Free, Writing, Pending, Reading };
    bool valid(int slot) const { return slot >= 0 && static_cast<size_t>(slot) < state_.size(); }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<State> state_;
    int pending_ = -1;
    bool closed_ = false;
    uint64_t refused_ = 0;
    uint64_t replaced_ = 0;
};

}  // namespace splat
