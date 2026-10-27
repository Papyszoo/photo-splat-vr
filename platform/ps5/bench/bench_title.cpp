// The benches' single homebrew title: read the bench selector from bench.env and
// call that bench's entry point.
//
// The title target (platform/ps5/title/) signs and packages this program; the
// three bench sources are linked into it and only their payload builds define
// main (they compile with -DPS5_TITLE here). The convert mode (convert_inbox.cpp)
// is title-only and adds a fourth selector value. Why one title and a selector
// rather than one title per program: docs/research/spike-a.md section 2.4.
//
//   /data/ps5-splat-vr/bench/bench.env
//   BENCH=op_audit            # or da2_bench, sharp_bench, convert, http
//
// A title gets no argv from ProsperoMgr's launch route (it calls
// sceSystemServiceLaunchApp with no argument block), so bench.env is the only
// input; run-bench.sh --title writes it before installing/launching. The rest
// of bench.env (driver settings) is passed through unchanged by the benches'
// load_env_file.
//
// The prologue runs before any bench code (splash down, stderr capture, quit
// watcher), and each bench's first act is to log the direct and flexible pools
// -- the number RADV's winsys will report as its one heap -- tagged with its
// own name, before its first GPU call.
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef PS5_TITLE

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include <sys/stat.h>

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include "ggml.h"

#include "bench_dispatch.h"
#include "bench_env.h"
#include "bench_title.h"

namespace splat {
namespace bench {

void title_vlog(const char* fmt, va_list args) {
    char message[1024];
    std::vsnprintf(message, sizeof(message), fmt, args);
    char line[1088];
    std::snprintf(line, sizeof(line), "%s\n", message);
    // One call per line: klog prefixes it with the process name, as it does for
    // a payload's klog_puts, so the runbook's greps are unchanged.
    sceKernelDebugOutText(0, line);
}

void title_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    title_vlog(fmt, args);
    va_end(args);
}

// The shared bench glue (platform/ps5/ggml/bench_env.cpp, compiled once for the
// payload route) writes its own log_line() through the payload SDK's
// klog_puts. A title imports no such module, so the title link needs this
// bridge; it puts the same text in klog. Title mode calls title_log instead,
// so this only carries a glue line that still uses the payload sink (and keeps
// the link honest rather than editing the shared glue).
extern "C" int klog_puts(const char* text) {
    char line[1088];
    std::snprintf(line, sizeof(line), "%s\n", text);
    sceKernelDebugOutText(0, line);
    return 0;
}

namespace {

double gib(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

void log_title_pools(const char* tag) {
    const int64_t direct = sceKernelGetDirectMemorySize();
    size_t        available = 0;
    int64_t       physical  = 0;
    const int32_t rv        = sceKernelAvailableDirectMemorySize(0, direct, 2 * 1024 * 1024,
                                                                 &physical, &available);
    title_log("%s: direct pool %lld bytes (%.2f GiB), available %zu bytes (%.2f GiB) rv=0x%08x",
              tag, (long long)direct, gib(direct), available, gib((int64_t)available),
              (uint32_t)rv);

    size_t        configured     = 0;
    size_t        free_flexible  = 0;
    const int32_t rv_configured  = sceKernelConfiguredFlexibleMemorySize(&configured);
    const int32_t rv_free        = sceKernelAvailableFlexibleMemorySize(&free_flexible);
    title_log("%s: flexible pool %zu MiB configured rv=0x%08x, %zu MiB free rv=0x%08x", tag,
              configured / (1024 * 1024), (uint32_t)rv_configured,
              free_flexible / (1024 * 1024), (uint32_t)rv_free);
}

void install_title_ggml_logging(const char* tag) {
    ggml_log_set(
        [](enum ggml_log_level level, const char* text, void* user_data) {
            const char* tag = static_cast<const char*>(user_data);
            const char* kind = "info";
            if (level == GGML_LOG_LEVEL_ERROR) kind = "error";
            else if (level == GGML_LOG_LEVEL_WARN) kind = "warn";
            else if (level == GGML_LOG_LEVEL_DEBUG) kind = "debug";
            // text already ends in a newline; keep one line per klog entry.
            char line[1200];
            std::snprintf(line, sizeof(line), "%s: ggml %s: %s", tag, kind, text);
            size_t end = std::strlen(line);
            while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r')) line[--end] = '\0';
            char out[1216];
            std::snprintf(out, sizeof(out), "%s\n", line);
            sceKernelDebugOutText(0, out);
        },
        const_cast<char*>(tag));
}

void start_quit_watch(const char* tag) {
    std::thread([tag] {
        const char* const path = "/data/ps5-splat-vr/title-quit";
        // Only a file written from now on is a request: a quit file left from a
        // previous stop must not end this run.
        const std::time_t started = std::time(nullptr);
        for (;;) {
            struct stat st{};
            if (::stat(path, &st) == 0 && st.st_mtime + 1 >= started) {
                title_log("%s: quit requested (%s); ending through LoadExec, no teardown", tag, path);
                sceSystemServiceLoadExec("exit", nullptr);
            }
            ::sleep(2);
        }
    }).detach();
}

}  // namespace bench
}  // namespace splat

namespace {

// The value of `key=` in the bench.env style file, or empty. Parsed here rather
// than through bench_env's loader so the selection happens before any bench
// applies a driver setting; the benches still load the whole file themselves.
std::string selector_value(const std::string& path, const char* key) {
    auto trim = [](std::string text) {
        const size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string();
        text.erase(0, first);
        const size_t last = text.find_last_not_of(" \t\r");
        text.erase(last + 1);
        return text;
    };
    const std::string text = splat::bench::read_text_file(path);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string entry = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (entry.empty() || entry[0] == '#') continue;
        const size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        if (trim(entry.substr(0, equals)) != key) continue;
        return trim(entry.substr(equals + 1));
    }
    return {};
}

}  // namespace

int main(int, char**) {
    splat::bench::title_log(
        "bench_title: start (ps5-splat-vr Spike A benches, one homebrew title, RADV linked directly)");
    sceSystemServiceHideSplashScreen();

    // RADV (and ACO) report failures on stderr, and nothing reads a title's
    // stderr; this puts each line in klog with the "[radv]" prefix the failure
    // table in docs/research/spike-a.md refers to (same call as vk_hello's
    // title build).
    splat::bench::title_log("bench_title: driver stderr capture %s",
                            ps5_klog_capture_stderr("[radv]") == 0 ? "on" : "off");

    // Our own stop route: title.sh stop writes the file and the title ends
    // itself (a title is never killed).
    splat::bench::start_quit_watch("bench_title");

    const std::string env_path = splat::bench::bench_path("bench.env");
    const std::string bench = selector_value(env_path, "BENCH");
    if (bench.empty()) {
        splat::bench::title_log(
            "bench_title: no BENCH= in %s; expected BENCH=op_audit, da2_bench, sharp_bench, "
            "convert or http (run-bench.sh --title or convert-inbox.sh writes it)", env_path.c_str());
        splat::bench::title_log("bench_title: FAIL setup, exit 2");
        sceSystemServiceLoadExec("exit", nullptr);
        return 2;
    }
    splat::bench::title_log("bench_title: bench %s (from %s)", bench.c_str(), env_path.c_str());

    // A title's argv is the app's, not ours (the launch route passes none), so
    // the benches see no arguments and use bench.env / the bench directory.
    // Each bench logs its own pools first (apply_bench_radv_defaults).
    int status = 2;
    if (bench == "op_audit") {
        status = op_audit_run(0, nullptr);
    } else if (bench == "da2_bench") {
        status = da2_bench_run(0, nullptr);
    } else if (bench == "sharp_bench") {
        status = sharp_bench_run(0, nullptr);
    } else if (bench == "convert") {
        // The inbox batch (platform/ps5/bench/convert_inbox.cpp): converts every
        // photo in /data/ps5-splat-vr/inbox that has no scene folder yet.
        status = convert_inbox_run(0, nullptr);
    } else if (bench == "http") {
        // The sceHttp/sceSsl/sceNet probe (platform/ps5/bench/http_probe.cpp):
        // HEAD, a 1 MiB ranged GET and a bounded text GET, no GPU.
        status = http_probe_run(0, nullptr);
    } else {
        splat::bench::title_log(
            "bench_title: unknown BENCH=%s in %s (op_audit, da2_bench, sharp_bench, convert, http)",
            bench.c_str(), env_path.c_str());
        splat::bench::title_log("bench_title: FAIL setup, exit 2");
    }

    splat::bench::title_log("bench_title: %s done, exit %d", bench.c_str(), status);
    // A title may not return to the CRT nor exit(): the system ends it through
    // LoadExec("exit"), which is what vk_hello's title build does too.
    sceSystemServiceLoadExec("exit", nullptr);
    return status;
}

#endif  // PS5_TITLE
