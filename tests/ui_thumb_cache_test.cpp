// ThumbCache: results arrive off the UI thread, a failed load is remembered,
// the LRU evicts the least recently used tile, and 200 interleaved
// request/get calls neither crash nor leave the cache over its capacity.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "ui/thumb_cache.h"

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, double seconds = 2.0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

splat::ThumbCache::Loader tile(uint32_t width, uint32_t height) {
    return [width, height] {
        splat::UiImage image;
        image.resize(width, height);
        return splat::Result<splat::UiImage>::success(std::move(image));
    };
}

}  // namespace

TEST_CASE("ui_thumb_cache delivers a loaded tile") {
    splat::ThumbCache cache(8);
    CHECK_FALSE(cache.pending());
    cache.request("scene-a", tile(3, 2));
    CHECK(cache.pending());
    REQUIRE(wait_until([&cache] { return !cache.pending(); }));

    const splat::UiImage* image = cache.get("scene-a");
    REQUIRE(image != nullptr);
    CHECK(image->w == 3);
    CHECK(image->h == 2);
    CHECK(cache.get("never-requested") == nullptr);
    CHECK(cache.size() == 1);
}

TEST_CASE("ui_thumb_cache remembers a failed load") {
    std::atomic<int> calls{0};
    splat::ThumbCache cache(8);
    cache.request("no-thumb", [&calls] {
        ++calls;
        return splat::Result<splat::UiImage>::failure("no thumb.jpg");
    });
    REQUIRE(wait_until([&cache] { return !cache.pending(); }));
    CHECK(cache.get("no-thumb") == nullptr);
    CHECK(calls.load() == 1);

    // Asking again does not retry the failing loader.
    cache.request("no-thumb", [&calls] {
        ++calls;
        return splat::Result<splat::UiImage>::success(splat::UiImage{});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(calls.load() == 1);
    CHECK_FALSE(cache.pending());
}

TEST_CASE("ui_thumb_cache evicts the least recently used tile") {
    splat::ThumbCache cache(4);
    const char* const keys[4] = {"a", "b", "c", "d"};
    for (const char* key : keys) cache.request(key, tile(1, 1));
    REQUIRE(wait_until([&cache] { return !cache.pending(); }));
    CHECK(cache.size() == 4);

    // Touch "a" so "b" becomes the least recently used tile.
    REQUIRE(cache.get("a") != nullptr);
    cache.request("e", tile(1, 1));
    REQUIRE(wait_until([&cache] { return cache.get("e") != nullptr; }));

    CHECK(cache.size() <= 4);
    CHECK(cache.get("a") != nullptr);
    CHECK(cache.get("b") == nullptr);
    CHECK(cache.get("c") != nullptr);
    CHECK(cache.get("d") != nullptr);
    CHECK(cache.get("e") != nullptr);
}

TEST_CASE("ui_thumb_cache survives 200 interleaved requests") {
    splat::ThumbCache cache(8, 200);
    std::atomic<int> loads{0};

    for (int index = 0; index < 200; ++index) {
        const std::string key = "k" + std::to_string(index % 16);
        cache.request(key, [&loads] {
            ++loads;
            splat::UiImage image;
            image.resize(1, 1);
            return splat::Result<splat::UiImage>::success(std::move(image));
        });
        // Reading a tile that is already cached must never block or crash.
        if (const splat::UiImage* image = cache.get(key)) CHECK(image->w == 1);
    }

    REQUIRE(wait_until([&cache] { return !cache.pending(); }, 5.0));
    CHECK(loads.load() <= 200);
    CHECK(loads.load() > 0);

    // A last lookup trims the cache back to capacity.
    cache.get("k15");
    CHECK(cache.size() <= 8);
}
