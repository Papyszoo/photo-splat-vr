// Asynchronous thumbnail cache shared by the Albums, Scenes and Photos screens.
//
// One worker thread decodes requested thumbnails; `get()` never blocks and only
// returns a tile that is already in memory. Results are kept in an LRU of
// `capacity` tiles (96 by default, enough for the visible grid plus a page
// either side). A key that failed is remembered, so a scene without thumb.jpg
// is not retried every frame.
//
// Threading contract: request() and get() are called from the UI thread, the
// worker only inserts finished tiles. Eviction happens in request()/get() on the
// caller's thread, so a pointer returned by get() stays valid until the next
// request()/get() call on that thread - use it within the frame. The destructor
// stops the worker and joins it; queued work is dropped.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/status.h"
#include "ui/image.h"

namespace splat {

class ThumbCache {
public:
    using Loader = std::function<Result<UiImage>()>;

    // `max_queue` bounds the requests waiting for the worker; a request beyond
    // it is dropped (the screen asks again next frame).
    explicit ThumbCache(std::size_t capacity = 96, std::size_t max_queue = 64);
    ~ThumbCache();

    ThumbCache(const ThumbCache&) = delete;
    ThumbCache& operator=(const ThumbCache&) = delete;

    // Queues `loader` for `key` unless the tile is cached, failed, or already
    // queued/loading. Does not block.
    void request(const std::string& key, Loader loader);

    // The cached tile, or null. Never blocks on the worker.
    const UiImage* get(const std::string& key) const;

    // True while a load is queued or running.
    bool pending() const;

    // Tiles currently held (after the last trim); for tests and diagnostics.
    std::size_t size() const;
    std::size_t capacity() const { return capacity_; }

private:
    struct Job {
        std::string key;
        Loader loader;
    };
    struct Entry {
        UiImage image;
        std::list<std::string>::iterator lru;
    };

    void worker();
    void touch_locked(const std::string& key) const;
    void trim_locked() const;

    const std::size_t capacity_;
    const std::size_t max_queue_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::unordered_set<std::string> inflight_;
    mutable std::unordered_map<std::string, Entry> cache_;
    mutable std::list<std::string> lru_;  // front = most recently used
    std::unordered_set<std::string> failed_;
    std::thread worker_;
    bool stop_ = false;
    bool busy_ = false;
};

}  // namespace splat
