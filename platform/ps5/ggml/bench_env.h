// Shared glue for the Spike A bench payloads (platform/ps5/bench/).
//
// A payload launched through ProsperoMgr gets no argv and no environment, so
// the benches read a small key=value file from the console bench directory
// before they touch the GPU. That is how a run selects a driver setting
// (GGML_VK_DISABLE_COOPMAT, RADV_DEBUG) without a rebuild:
//
//   /data/ps5-splat-vr/bench/bench.env
//   # comments and blank lines are ignored
//   GGML_VK_DISABLE_COOPMAT=1
//
// Everything here is console-side: logging to stdout and the kernel log, a
// monotonic clock, peak RSS, and a watchdog for the one thing ggml cannot
// bound on its own (see Watchdog below).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace splat {
namespace bench {

// The console directory the benches read from and write next to. Overridable
// through PS5_BENCH_DIR (bench.env or the real environment).
extern const char* const kDefaultBenchDir;

// printf to stdout and klog_puts, one line, flushed. `tag` is prefixed by the
// caller (each payload uses its ELF stem).
void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

double now_seconds();
inline double now_ms() { return now_seconds() * 1000.0; }

// "1.31 GiB" style. Rounds to two decimals in the largest binary unit.
std::string human_bytes(uint64_t bytes);

// Process high-water RSS. FreeBSD reports getrusage's ru_maxrss in KiB.
uint64_t peak_rss_bytes();

// The two RADV settings vk_hello makes before its first GPU call
// (docs/research/spike-b.md §4), applied without overwriting: a payload has no
// title folder for RADV's shader cache, and without the unified-heap flag RADV
// splits the APU pool into ~7.6 + ~3.8 GiB instead of one ~11.44 GiB heap.
// Call before load_env_file() so bench.env can still override either.
void apply_radv_defaults(const char* tag);

// Reads a key=value file and exports every entry with setenv(3). Returns the
// number applied; a missing file is not an error (returns 0 after logging).
// Must run before the first Vulkan call for driver settings to take effect.
int load_env_file(const std::string& path, const char* tag);

// Reads a file into memory; empty on failure.
std::string read_text_file(const std::string& path);

// The console bench directory: PS5_BENCH_DIR, else kDefaultBenchDir.
std::string bench_dir();

// Resolves `name` inside the bench directory (absolute names pass through).
std::string bench_path(const std::string& name);

// First existing path from a list, or "".
std::string first_existing(const std::string& a, const std::string& b = std::string());

// GPU submissions observed by the link: every ggml_backend_graph_compute call
// is counted by a --wrap'd wrapper in bench_env.cpp. The reconstruction code
// submits one small graph per stage/checkpoint (GraphSession), so this is the
// submit count the device-lost risk is about -- without an API change in core/.
void reset_submits();
int submits();
// Internal: called by the --wrap'd graph submission.
void note_submit();

// ---- watchdog -------------------------------------------------------------
//
// ggml-vulkan waits for its fences with an infinite timeout
// (vkWaitForFences(..., UINT64_MAX) in ggml-vulkan.cpp). A device lost or a
// wedged submission would therefore hang the payload forever with no log line,
// which is exactly the failure the console run is looking for. arm() before a
// wait (graph compute, synchronize, model load, inference, scene write),
// disarm() after; if the deadline passes the watchdog thread logs what was in
// flight and calls _exit(3) without tearing anything down — the same "leak, do
// not free under a GPU that may still run" policy vk_hello uses
// (docs/research/spike-b.md §4). Nothing is destroyed while a submission may
// still be reading it.
class Watchdog {
public:
    explicit Watchdog(const char* tag);
    ~Watchdog();

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // `label` must stay valid until disarm() (use a string literal or a static).
    void arm(double timeout_seconds, const char* label);
    void disarm();

    bool fired() const { return fired_; }

private:
    void run();
    struct Impl;
    Impl* impl_ = nullptr;
    const char* tag_ = "bench";
    volatile bool fired_ = false;
};

// ---- coopmat on/off in one process ----------------------------------------
//
// ggml-vulkan decides cooperative-matrix use once, when it builds the cached
// VkDevice for physical index 0, and keeps that device for the whole process.
// op_audit measures every case at the bench.env setting and again with
// GGML_VK_DISABLE_COOPMAT flipped, so between the passes the cached device is
// released and rebuilt. platform/ps5/ggml/vk_reset.cpp does that through
// ggml's own vk_instance and is part of the PS5 glue library. The host smoke
// build's ggml dylib hides vk_instance, so it defines SPLAT_BENCH_NO_VK_RESET
// and op_audit logs a single pass there.
//
// vk_device_coopmat returns 1 (coopmat1 exposed), 0 (not exposed) or -1 (no
// device / build without the glue).
bool vk_reset_available();
int vk_device_coopmat(int idx);
bool vk_release_device(int idx);

}  // namespace bench
}  // namespace splat
