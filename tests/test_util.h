#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <doctest/doctest.h>

#include "common/image.h"
#include "jobs/image_io.h"
#include "scene/splat_scene.h"

namespace splat_test {

// Removes itself at scope exit, so failing REQUIREs do not leak temp folders.
class TempDir {
public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = base / (std::string("psplat-test-") + tag + "-" +
                            std::to_string(counter.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directories(path_, error)) return;
            if (error && attempt == 99) {
                throw std::runtime_error("cannot create temp dir: " + error.message());
            }
        }
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Deterministic scene with every attribute varying, in a range that exercises
// fp16 normal and subnormal values.
inline splat::SplatScene make_test_scene(size_t count) {
    splat::SplatScene scene;
    scene.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i);
        scene.position[i * 3 + 0] = 0.37f * t - 1.5f;
        scene.position[i * 3 + 1] = std::sin(t) * 2.5f;
        scene.position[i * 3 + 2] = 1.5f + 0.01f * t * t;
        scene.log_scale[i * 3 + 0] = -4.0f + 0.03f * t;
        scene.log_scale[i * 3 + 1] = -3.5f + 0.02f * t;
        scene.log_scale[i * 3 + 2] = -5.0f + 0.01f * t;
        scene.rotation[i * 4 + 0] = 0.9f - 0.01f * t;
        scene.rotation[i * 4 + 1] = 0.1f * t;
        scene.rotation[i * 4 + 2] = -0.05f * t;
        scene.rotation[i * 4 + 3] = 0.02f * t;
        scene.sh_dc[i * 3 + 0] = -1.2f + 0.01f * t;
        scene.sh_dc[i * 3 + 1] = 0.3f - 0.02f * t;
        scene.sh_dc[i * 3 + 2] = 1.1f - 0.03f * t;
        scene.opacity_logit[i] = -4.0f + 0.05f * t;
    }
    return scene;
}

// fp16 has ~11 bits of mantissa, so 2e-3 relative is a safe round-trip bound.
inline bool close_enough(float a, float b, float relative = 2e-3f) {
    if (std::isinf(a) || std::isinf(b)) return a == b;
    const float scale = std::max(1.0f, std::max(std::abs(a), std::abs(b)));
    return std::abs(a - b) <= relative * scale;
}

// A deterministic JPEG for the job queues (core/jobs reads the file, the EXIF
// reader finds no focal length and the runner's 30 mm default applies). Shared
// by tests/ui_jobservice_test.cpp and tests/ps5_jobs_order_test.cpp, which both
// used to carry this copy.
inline splat::Path write_test_jpeg(const splat::Path& path, int width = 32, int height = 24) {
    splat::Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            pixel[0] = static_cast<uint8_t>((x * 7 + y) & 0xff);
            pixel[1] = static_cast<uint8_t>((y * 5) & 0xff);
            pixel[2] = static_cast<uint8_t>((x * 3) & 0xff);
        }
    }
    const auto encoded = splat::encode_jpeg(image, 90);
    REQUIRE_MESSAGE(encoded.ok(), encoded.message());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(encoded.value().data()),
              static_cast<std::streamsize>(encoded.value().size()));
    return path;
}

// Polls a predicate for up to `timeout`; returns its final value. Shared by
// tests/ui_jobservice_test.cpp and tests/ps5_jobs_order_test.cpp.
inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

// Wakes a fake backend that is holding inside load()/run(). The state carries a
// mutex, a condition_variable and a `released` flag (tests/ui_jobservice_test.cpp's
// FakeState, tests/ps5_jobs_order_test.cpp's LoggingReconstructor::Control), so
// one helper releases either.
template <typename State>
void release(const std::shared_ptr<State>& state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
    }
    state->cv.notify_all();
}

}  // namespace splat_test
