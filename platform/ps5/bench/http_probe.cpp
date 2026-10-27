// BENCH=http: does a title's sceHttp stack work on this console?
//
// The console has no libcurl. The UI's model download needs HEAD, a ranged GET
// to a file and a bounded text GET, so this mode exercises exactly those three
// against the same pinned DA2-Small URL the model manager uses, without
// touching the GPU or the model manager:
//
//   head       status, content length, Accept-Ranges
//   download   the first 1 MiB with a Range request, appended to
//              /data/ps5-splat-vr/state/http_probe.bin (the only write)
//   get_text   2 KB of the same host's /robots.txt, skipped silently when the
//              host has none (a 404 is not a failure of the stack)
//
// It is a title mode (BENCH=http in /data/ps5-splat-vr/bench/bench.env), like
// convert: sceHttp is the part that is unproven from a title, so a payload
// would test the wrong thing.
//
//   HTTP_URL=<url>        # optional; defaults to the pinned DA2-Small URL
//
// Exit: 0 PASS, 2 a failed stage. The title ends through LoadExec either way.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>

#include "bench_env.h"
#include "bench_title.h"

#include "common/path.h"
#include "common/status.h"
#include "model/da2_profile.h"
#include "model/sha256.h"
#include "platform/ps5/net/ps5_http_fetcher.h"

using splat::HttpResource;
using splat::Path;
using splat::Ps5HttpFetcher;
using splat::Result;
using splat::Status;
using splat::bench::Watchdog;

namespace {

const char* const kTag = "http";
constexpr uint64_t kProbeBytes = 1024 * 1024;
constexpr size_t kTextBytes = 2048;
// The one path this mode writes; the state directory is ours (docs/PLAN.md).
const char* const kProbePath = "/data/ps5-splat-vr/state/http_probe.bin";

#ifdef PS5_TITLE
void
log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void
log_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    splat::bench::title_vlog(fmt, args);
    va_end(args);
}
#else
using splat::bench::log_line;
#endif

std::string env_value(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

// "https://host/path" -> "https://host"; empty when there is no scheme.
std::string origin_of(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t host_start = scheme + 3;
    const size_t end = url.find_first_of("/?#", host_start);
    return end == std::string::npos ? url : url.substr(0, end);
}

int fail(const char* where, const std::string& message) {
    log_line("%s: FAIL %s: %s", kTag, where, message.c_str());
    return 2;
}

}  // namespace

int http_probe_run(int, char**) {
    Watchdog watchdog(kTag);
    splat::bench::apply_bench_radv_defaults(kTag);
    splat::bench::load_env_file(splat::bench::bench_path("bench.env"), kTag);

    const std::string url = env_value("HTTP_URL", splat::kDepthAnythingV2SmallUrl);
    log_line("%s: url %s", kTag, url.c_str());

    Ps5HttpFetcher fetcher;

    watchdog.arm(120.0, "head");
    const Result<HttpResource> head = fetcher.head(url);
    watchdog.disarm();
    if (!head.ok()) return fail("head", head.message());
    log_line("%s: head %ld length %llu ranges %d", kTag, fetcher.last_status(),
             static_cast<unsigned long long>(head.value().content_length),
             head.value().range_supported ? 1 : 0);

    const Path probe(kProbePath);
    watchdog.arm(300.0, "download");
    const Status downloaded =
        fetcher.download_range(url, probe, 0, kProbeBytes, nullptr, std::function<bool()>());
    watchdog.disarm();
    if (!downloaded.ok()) return fail("download", downloaded.message());

    std::error_code size_error;
    const uint64_t probe_size = std::filesystem::file_size(probe, size_error);
    if (size_error) return fail("download", "cannot read back " + probe.string() + ": " + size_error.message());
    const Result<std::string> digest = splat::sha256_file(probe);
    if (!digest.ok()) return fail("sha256", digest.message());
    log_line("%s: probe %llu bytes sha256 %s", kTag, static_cast<unsigned long long>(probe_size),
             digest.value().c_str());

    // The licence GET (LICENSE_MODEL) is the same shape: a bounded text body
    // from the checkpoint's host. robots.txt is the harmless equivalent here.
    const std::string origin = origin_of(url);
    if (!origin.empty()) {
        watchdog.arm(60.0, "get_text");
        const Result<std::string> text = fetcher.get_text(origin + "/robots.txt", kTextBytes);
        watchdog.disarm();
        // A host without robots.txt answers 404; that is not a stack failure.
        if (text.ok()) log_line("%s: robots %zu bytes", kTag, text.value().size());
    }

    log_line("%s: PASS", kTag);
    return 0;
}
