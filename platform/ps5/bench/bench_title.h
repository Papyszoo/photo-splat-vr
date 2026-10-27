// Title-mode helpers for the Spike A benches (and the title-only convert mode).
//
// The benches are one homebrew title (platform/ps5/bench, the one app: title id
// in platform/ps5/title/param.json): the title's program reads
// BENCH=op_audit|da2_bench|sharp_bench|convert from
// /data/ps5-splat-vr/bench/bench.env and calls that entry point. Why one title
// with a selector instead of one package per program is in
// docs/research/spike-a.md section 2.4.
//
// A payload has no direct-memory pool, so RADV gives it no heaps and every
// bench allocation fails; that is the whole reason for this file
// (docs/research/spike-b-memory.md sections 1-2). What a title changes:
//
//   - logging goes through sceKernelDebugOutText, because nothing reads a
//     title's stdout; the message text is unchanged, so every grep and expected
//     line in docs/research/spike-a.md section 7 still applies;
//   - the direct + flexible pools are logged before the first Vulkan call: the
//     direct pool is the number RADV's PS5 winsys turns into its one heap;
//   - RADV's shader cache is left at the title default (a payload must disable
//     it -- it has no title folder to put the cache in);
//   - the process ends with sceSystemServiceLoadExec("exit", nullptr): a title
//     may not exit() (SIGSYS), exactly as vk_hello's title build does.
//
// The bounded-wait policy does not change: the benches keep the shared
// Watchdog from bench_env.h and abandon the process instead of tearing down
// GPU memory that may still be in use.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <unistd.h>

#include "bench_env.h"

#ifdef PS5_TITLE
#include <ps5platform/kernel.h>  // direct/flexible pool, sceKernelDebugOutText
#include <ps5platform/klog.h>

extern "C" int sceSystemServiceHideSplashScreen(void);
// How a title process ends: a title may not exit() (SIGSYS).
extern "C" int sceSystemServiceLoadExec(const char* path, const char* const* args);
#endif

namespace splat {
namespace bench {

#ifdef PS5_TITLE
// printf to the kernel log, one line, flushed. `fmt` carries the tag the caller
// already prefixes, so the text matches the payload build's log_line().
void title_vlog(const char* fmt, va_list args);
void title_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// The title's two pools, read never assumed (docs/research/spike-b-memory.md
// sections 1-2). Logged with the selected bench's tag before its first GPU
// call, so the pool is the first number in the bench's log.
void log_title_pools(const char* tag);
#endif

// The two RADV settings a bench makes before its first GPU call
// (docs/research/spike-b.md section 4), without overwriting either: bench.env
// (loaded next) still wins. A payload has no title folder for RADV's cache, so
// it disables the cache; a title keeps it. Both set the unified-heap flag so
// RADV reports the direct pool as one ~11.44 GiB heap instead of splitting it.
inline void apply_bench_radv_defaults(const char* tag) {
#ifdef PS5_TITLE
    // First line the bench writes: the pools RADV's winsys will see. A title's
    // direct pool is what becomes the one device heap, so it comes before the
    // first GPU call and before any driver setting.
    log_title_pools(tag);
    const int heap = setenv("radv_enable_unified_heap_on_apu", "true", 0);
    title_log("%s: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)%s",
              tag, heap == 0 ? "" : " [setenv failed]");
#else
    apply_radv_defaults(tag);
#endif
}

#ifdef PS5_TITLE
// ggml's own log lines (the device description and its `matrix cores:` field)
// through klog. The payload build uses platform/ps5/ggml/ggml_logging.h, whose
// sink is klog_puts -- the payload route, which a title does not have.
void install_title_ggml_logging(const char* tag);

// Polls /data/ps5-splat-vr/title-quit, the graceful stop `title.sh stop`
// writes. A request ends the process through LoadExec without any teardown:
// the GPU may still be running, so nothing is freed (the vk_hello policy).
void start_quit_watch(const char* tag);
#endif

// Ends the process after a failure that may have left work on the GPU: no
// teardown, no destructors. A payload flushes stdout and _exit()s. A title has
// no stdout and may not exit(), so it logs the same line and LoadExec()s
// ("exit") -- the process end a clean title run uses too.
[[noreturn]] inline void abandon(const char* tag, int code) {
#ifdef PS5_TITLE
    title_log("%s: abandoning the process (exit %d, no teardown; the GPU may still run)", tag, code);
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) ::sleep(1);  // LoadExec ends the app; never return to the CRT
#else
    (void)tag;  // a payload's log line already said what failed
    std::fflush(stdout);
    ::_exit(code);
#endif
}

}  // namespace bench
}  // namespace splat
