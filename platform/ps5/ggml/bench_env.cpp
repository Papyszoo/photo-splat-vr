#include "bench_env.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ps5/klog.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

#include <chrono>

#include "ggml-backend.h"

namespace splat {
namespace bench {

const char* const kDefaultBenchDir = "/data/ps5-splat-vr/bench";

void log_line(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

std::string human_bytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f %s", value, kUnits[unit]);
    return text;
}

uint64_t peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    // macOS reports bytes; FreeBSD (the PS5 kernel) and Linux report kilobytes.
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024ull;
#endif
}

static std::string trim(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

void apply_radv_defaults(const char* tag) {
    // overwrite = 0: a bench.env entry (loaded next) or the real environment wins.
    const int cache = setenv("MESA_SHADER_CACHE_DISABLE", "true", 0);
    const int heap = setenv("radv_enable_unified_heap_on_apu", "true", 0);
    log_line("%s: MESA_SHADER_CACHE_DISABLE=true (no title folder for RADV's cache)%s",
             tag, cache == 0 ? "" : " [setenv failed]");
    log_line("%s: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)%s",
             tag, heap == 0 ? "" : " [setenv failed]");
}

int load_env_file(const std::string& path, const char* tag) {
    std::ifstream file(path);
    if (!file) {
        log_line("%s: no env file at %s (driver defaults)", tag, path.c_str());
        return 0;
    }
    int applied = 0;
    std::string line;
    while (std::getline(file, line)) {
        const std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#') continue;
        const size_t equals = entry.find('=');
        if (equals == std::string::npos || equals == 0) {
            log_line("%s: env: ignoring %s", tag, entry.c_str());
            continue;
        }
        const std::string key = trim(entry.substr(0, equals));
        const std::string value = trim(entry.substr(equals + 1));
        if (setenv(key.c_str(), value.c_str(), 1) != 0) {
            log_line("%s: env: setenv(%s) failed", tag, key.c_str());
            continue;
        }
        ++applied;
        // Values that select a code path (coopmat, debug) are worth a line;
        // secrets are not a thing in this file.
        log_line("%s: env %s=%s", tag, key.c_str(), value.c_str());
    }
    log_line("%s: env file %s: %d entries", tag, path.c_str(), applied);
    return applied;
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return text;
}

std::string bench_dir() {
    const char* env = std::getenv("PS5_BENCH_DIR");
    if (env != nullptr && env[0] != '\0') return env;
    return kDefaultBenchDir;
}

std::string bench_path(const std::string& name) {
    if (!name.empty() && name[0] == '/') return name;
    std::string dir = bench_dir();
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir + "/" + name;
}

std::string first_existing(const std::string& a, const std::string& b) {
    struct stat st{};
    if (!a.empty() && ::stat(a.c_str(), &st) == 0) return a;
    if (!b.empty() && ::stat(b.c_str(), &st) == 0) return b;
    return {};
}

// ---- submit counter -------------------------------------------------------
//
// The payloads link with -Wl,--wrap=ggml_backend_graph_compute, so the
// reconstruction code's calls land here instead of directly in ggml-base.
// One call == one submission (GraphSession submits one small graph at a time).

namespace {
std::atomic<int> g_submits{0};
}

void reset_submits() { g_submits.store(0); }
int submits() { return g_submits.load(); }
void note_submit() { g_submits.fetch_add(1); }

// ---- watchdog -------------------------------------------------------------

struct Watchdog::Impl {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool armed = false;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point deadline;
    const char* label = "";
};

Watchdog::Watchdog(const char* tag) : impl_(new Impl()), tag_(tag) {
    impl_->thread = std::thread([this] { run(); });
}

Watchdog::~Watchdog() {
    if (impl_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    delete impl_;
    impl_ = nullptr;
}

void Watchdog::arm(double timeout_seconds, const char* label) {
    if (impl_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->label = label;
        impl_->deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int64_t>(timeout_seconds * 1000.0));
        impl_->armed = true;
        ++impl_->generation;
    }
    impl_->cv.notify_all();
}

void Watchdog::disarm() {
    if (impl_ == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->armed = false;
        ++impl_->generation;
    }
    impl_->cv.notify_all();
}

void Watchdog::run() {
    Impl& self = *impl_;
    for (;;) {
        std::unique_lock<std::mutex> lock(self.mutex);
        self.cv.wait(lock, [&self] { return self.stop || self.armed; });
        if (self.stop) return;
        const uint64_t generation = self.generation;
        const auto deadline = self.deadline;
        const char* label = self.label;
        const bool changed = self.cv.wait_until(lock, deadline, [&self, generation] {
            return self.stop || !self.armed || self.generation != generation;
        });

        if (changed) continue;
        lock.unlock();
        fired_ = true;
        // Nothing is destroyed: the submission may still be running.
        log_line("%s: WATCHDOG %s exceeded its deadline; the GPU may still run", tag_, label);
        log_line("%s: WATCHDOG abandoning the payload (exit 3, no teardown)", tag_);
        ::_exit(3);
    }
}

}  // namespace bench
}  // namespace splat

#ifdef SPLAT_BENCH_NO_VK_RESET
// A host smoke build that does not link platform/ps5/ggml/vk_reset.cpp: op_audit
// cannot release the cached device and logs one pass.
namespace splat {
namespace bench {
bool vk_reset_available() { return false; }
int vk_device_coopmat(int) { return -1; }
bool vk_release_device(int) { return false; }
}  // namespace bench
}  // namespace splat
#endif

#ifndef SPLAT_BENCH_NO_WRAP
// The payloads are linked with -Wl,--wrap=ggml_backend_graph_compute, so every
// graph submission from core/ passes through here first. (macOS ld64 has no
// --wrap; a host smoke build defines SPLAT_BENCH_NO_WRAP and reports 0 submits.)
extern "C" ggml_status __real_ggml_backend_graph_compute(ggml_backend_t backend,
                                                         struct ggml_cgraph* graph);

extern "C" ggml_status __wrap_ggml_backend_graph_compute(ggml_backend_t backend,
                                                         struct ggml_cgraph* graph) {
    splat::bench::note_submit();
    return __real_ggml_backend_graph_compute(backend, graph);
}
#endif
