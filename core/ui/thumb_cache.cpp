// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/thumb_cache.h"

#include <utility>

namespace splat {

ThumbCache::ThumbCache(std::size_t capacity, std::size_t max_queue)
    : capacity_(capacity == 0 ? 1 : capacity), max_queue_(max_queue) {
    worker_ = std::thread([this] { worker(); });
}

ThumbCache::~ThumbCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
        inflight_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ThumbCache::request(const std::string& key, Loader loader) {
    if (key.empty() || !loader) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.find(key) != cache_.end()) {
            touch_locked(key);
            return;
        }
        if (failed_.find(key) != failed_.end()) return;
        if (inflight_.find(key) != inflight_.end()) return;
        if (queue_.size() >= max_queue_) return;
        queue_.push_back(Job{key, std::move(loader)});
        inflight_.insert(key);
        trim_locked();
    }
    wake_.notify_one();
}

const UiImage* ThumbCache::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = cache_.find(key);
    if (found == cache_.end()) {
        trim_locked();
        return nullptr;
    }
    // Touch first so trimming cannot evict the tile being looked up.
    touch_locked(key);
    trim_locked();
    return &cache_.find(key)->second.image;
}

bool ThumbCache::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_ || !queue_.empty();
}

std::size_t ThumbCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void ThumbCache::touch_locked(const std::string& key) const {
    const auto found = cache_.find(key);
    if (found == cache_.end()) return;
    lru_.splice(lru_.begin(), lru_, found->second.lru);
}

void ThumbCache::trim_locked() const {
    while (cache_.size() > capacity_ && !lru_.empty()) {
        const std::string victim = lru_.back();
        lru_.pop_back();
        cache_.erase(victim);
    }
}

void ThumbCache::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }

        Result<UiImage> loaded = Result<UiImage>::failure("no loader");
        try {
            if (job.loader) loaded = job.loader();
        } catch (...) {
            loaded = Result<UiImage>::failure("thumbnail loader threw");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            inflight_.erase(job.key);
            if (loaded.ok()) {
                UiImage image = loaded.take();
                const auto found = cache_.find(job.key);
                if (found == cache_.end()) {
                    lru_.push_front(job.key);
                    cache_.emplace(job.key, Entry{std::move(image), lru_.begin()});
                } else {
                    found->second.image = std::move(image);
                    touch_locked(job.key);
                }
            } else {
                failed_.insert(job.key);
            }
        }
    }
}

}  // namespace splat
