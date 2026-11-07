// BENCH=http: does a title's sceHttp stack work on this console, and how fast
// can it download?
//
// The console has no libcurl. Two modes, both title-only (BENCH=http in
// /data/ps5-splat-vr/bench/bench.env, like convert): sceHttp is the part that
// is unproven from a title, so a payload would test the wrong thing.
//
// 1. Download speed (the default; 0.6.0, runs/v0.2-library/REPORT-060-download.md).
//    The model download measured 2-3 MB/s on the console against 15-19 MB/s for
//    one connection on the Mac, and the cause is unproven. This times ranged
//    GETs of the pinned SHARP checkpoint, each on a fresh connection and on its
//    own byte range (so the CDN edge cache cannot help), writing to the bench
//    directory and deleting the files afterwards:
//
//      A  platform/ps5/net/ps5_http_fetcher.cpp as it was through 0.5.x (the
//         baseline; 0.6.0 moved the fetcher to the C settings): its pools,
//         64 KiB reads, one connection, HTTP/1.1, sceHttp's default receive
//         block.
//      B  A plus sceHttpSetRecvBlockSize, 2 MiB / 1 MiB / 2 MiB pools and
//         256 KiB reads.
//      C  B settings on 4 threads, one request on its own range each; the
//         requests start reading together and the rate is the sum.
//
//    Each variant ends with one `http: result variant=...` line: bytes, seconds,
//    MB/s (1e6 bytes), microseconds inside sceHttpReadData vs fwrite, the
//    connect time, the stop reason (time, cap, eof, error) and any error code.
//    At most 188 MB are written in total (caps per request: A 40, B 60, C 4x22).
//
//      HTTP_URL=<url>        # default: the pinned SHARP checkpoint
//      HTTP_OFFSET=<bytes>   # default 1000000000; use another value per re-run
//      HTTP_SECONDS=<n>      # per variant, default 20 (1..120)
//      HTTP_VARIANT=A|B|C|all  # default all, run in that order
//      HTTP_BLOCK=<bytes>    # B and C receive block size; default tries
//                            # 262144, 65536, 16384 until the call succeeds
//
// 2. Stack smoke test (HTTP_VARIANT=smoke): the three calls the UI's model
//    download needs, against the DA2-Small URL the model manager uses, without
//    touching the GPU or the model manager:
//
//      head       status, content length, Accept-Ranges
//      download   the first 1 MiB with a Range request, appended to
//                 /data/ps5-splat-vr/state/http_probe.bin
//      get_text   2 KB of the same host's /robots.txt, skipped silently when the
//                 host has none (a 404 is not a failure of the stack)
//
// Exit: 0 PASS, 2 a failed stage. The title ends through LoadExec either way.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "bench_env.h"
#include "bench_title.h"

#include "common/path.h"
#include "common/status.h"
#include "model/da2_profile.h"
#include "model/sha256.h"
#include "platform/ps5/net/http_range.h"
#include "platform/ps5/net/ps5_http_fetcher.h"
#include "platform/ps5/net/sce_http.h"

// In libSceHttp's stub and never called by the app (ProsperoMgr does not call it
// either), so sce_http.h has no prototype. `id` is a template, connection or
// request id; sceHttp's own default block is small (documented as 1500 bytes).
extern "C" int sceHttpSetRecvBlockSize(int id, unsigned int blockSize);

using splat::HttpResource;
using splat::Path;
using splat::Ps5HttpFetcher;
using splat::Result;
using splat::Status;
using splat::bench::Watchdog;

namespace {

using Clock = std::chrono::steady_clock;

const char* const kTag = "http";
constexpr uint64_t kProbeBytes = 1024 * 1024;
constexpr size_t kTextBytes = 2048;
// The one path the smoke mode writes; the state directory is ours (docs/PLAN.md).
const char* const kProbePath = "/data/ps5-splat-vr/state/http_probe.bin";

// The speed mode's target: the pinned SHARP checkpoint (docs/spec.md rule 2,
// core/model/model_manager.cpp sharp_model_spec()), 2.8 GB behind the Apple CDN.
const char* const kSharpUrl = "https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt";
constexpr uint64_t kMB = 1000000ull;
// Each request gets its own slot of the file, so no byte is fetched twice in a
// run: A slot 0, B slot 1, C threads slots 2..5. The farthest range ends below
// HTTP_OFFSET + 6 * kSlotStride.
constexpr uint64_t kSlotStride = 100 * kMB;
constexpr int kSlotCount = 6;
// Same values as platform/ps5/net/ps5_http_fetcher.cpp (variant A is the 0.5.x
// baseline; the fetcher now runs the C settings).
constexpr int kMethodGet = 0;
constexpr unsigned int kConnectTimeoutUs = 15u * 1000u * 1000u;
constexpr unsigned int kReceiveTimeoutUs = 30u * 1000u * 1000u;
// Receive block sizes tried for B and C, largest first, until one is accepted.
const unsigned int kBlockLadder[] = {262144u, 65536u, 16384u};

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

std::string upper_ascii(std::string text) {
    for (char& character : text) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    }
    return text;
}

std::string hex_code(int code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned int>(code));
    return text;
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

// ---- speed mode -----------------------------------------------------------------

// One variant's settings. A is what ps5_http_fetcher.cpp did through 0.5.x.
struct Knobs {
    const char* name;         // "A"
    const char* watch_label;  // the watchdog keeps the pointer
    int net_pool;
    int ssl_pool;
    int http_pool;
    size_t chunk;        // bytes asked of one sceHttpReadData
    bool recv_block;     // call sceHttpSetRecvBlockSize
    int threads;         // requests in flight, one per thread
    uint64_t cap_bytes;  // per request; the sum over all variants is 188 MB
    int first_slot;
};

constexpr Knobs kKnobsA = {"A", "http variant A", 512 * 1024, 384 * 1024, 256 * 1024, 64 * 1024,
                           false, 1, 40 * kMB, 0};
constexpr Knobs kKnobsB = {"B", "http variant B", 2 * 1024 * 1024, 1024 * 1024, 2 * 1024 * 1024,
                           256 * 1024, true, 1, 60 * kMB, 1};
constexpr Knobs kKnobsC = {"C", "http variant C", 2 * 1024 * 1024, 1024 * 1024, 2 * 1024 * 1024,
                           256 * 1024, true, 4, 22 * kMB, 2};

// One net/ssl/http/template set, built exactly like make_context() in
// platform/ps5/net/ps5_http_fetcher.cpp apart from the pool sizes and the
// receive block. A variant owns a fresh one and tears it down in the order
// ProsperoMgr's http_ctx_close() uses on hardware.
struct Context {
    int net_pool = -1;
    int ssl_ctx = -1;
    int http_ctx = -1;
    int tmpl = -1;
    unsigned int block = 0;  // accepted receive block, 0 = sceHttp's default
    std::string error;
};

void set_timeouts(int id) {
    sceHttpSetResolveTimeOut(id, kConnectTimeoutUs);
    sceHttpSetConnectTimeOut(id, kConnectTimeoutUs);
    sceHttpSetSendTimeOut(id, kReceiveTimeoutUs);
    sceHttpSetRecvTimeOut(id, kReceiveTimeoutUs);
}

Context make_context(const Knobs& knobs, unsigned int forced_block) {
    Context context;
    if (sceNetInit() < 0) {
        context.error = "sceNetInit failed";
        return context;
    }
    const std::string pool_name = std::string("splat_probe_") + knobs.name;
    context.net_pool = sceNetPoolCreate(pool_name.c_str(), knobs.net_pool, 0);
    if (context.net_pool < 0) {
        context.error = "sceNetPoolCreate failed (" + hex_code(context.net_pool) + ")";
        return context;
    }
    context.ssl_ctx = sceSslInit(static_cast<size_t>(knobs.ssl_pool));
    if (context.ssl_ctx < 0) {
        context.error = "sceSslInit failed (" + hex_code(context.ssl_ctx) + ")";
        return context;
    }
    context.http_ctx = sceHttpInit(context.net_pool, context.ssl_ctx, static_cast<size_t>(knobs.http_pool));
    if (context.http_ctx < 0) {
        context.error = "sceHttpInit failed (" + hex_code(context.http_ctx) + ")";
        return context;
    }
    context.tmpl = sceHttpCreateTemplate(context.http_ctx, "ps5-splat-vr/1.0", 2, 1);
    if (context.tmpl < 0) {
        context.error = "sceHttpCreateTemplate failed (" + hex_code(context.tmpl) + ")";
        return context;
    }
    sceHttpSetResponseHeaderMaxSize(context.tmpl, 0x4000);
    const int redirect = sceHttpSetAutoRedirect(context.tmpl, 1);
    if (redirect < 0) {
        context.error = "sceHttpSetAutoRedirect failed (" + hex_code(redirect) + ")";
        return context;
    }
    set_timeouts(context.tmpl);

    if (knobs.recv_block) {
        // The first value sceHttp accepts stays; every attempt is logged.
        std::vector<unsigned int> candidates;
        if (forced_block != 0) {
            candidates.push_back(forced_block);
        } else {
            candidates.assign(std::begin(kBlockLadder), std::end(kBlockLadder));
        }
        for (const unsigned int block : candidates) {
            const int rc = sceHttpSetRecvBlockSize(context.tmpl, block);
            log_line("%s: %s sceHttpSetRecvBlockSize(template, %u) = %s", kTag, knobs.name, block,
                     hex_code(rc).c_str());
            if (rc == 0) {
                context.block = block;
                break;
            }
        }
    }
    return context;
}

void close_context(Context& context) {
    if (context.tmpl >= 0) sceHttpDeleteTemplate(context.tmpl);
    if (context.http_ctx >= 0) sceHttpTerm(context.http_ctx);
    if (context.ssl_ctx >= 0) sceSslTerm(context.ssl_ctx);
    if (context.net_pool >= 0) sceNetPoolDestroy(context.net_pool);
    context = Context();
}

struct Req {
    int connection = -1;
    int request = -1;

    Req() = default;
    ~Req() { reset(); }
    Req(const Req&) = delete;
    Req& operator=(const Req&) = delete;

    void reset() {
        if (request >= 0) sceHttpDeleteRequest(request);
        if (connection >= 0) sceHttpDeleteConnection(connection);
        request = -1;
        connection = -1;
    }
};

// One request on its own byte range and what it measured.
struct Transfer {
    uint64_t offset = 0;
    std::string path;
    uint64_t bytes = 0;
    uint64_t reads = 0;
    long status = 0;
    int64_t connect_us = 0;  // connection + request + send + headers
    int64_t body_us = 0;     // barrier release .. last read (incl. the final flush)
    int64_t read_us = 0;     // inside sceHttpReadData
    int64_t write_us = 0;    // inside fwrite/fflush/fclose
    int64_t close_us = 0;    // deleting the request and connection
    int rc_conn = 0;         // sceHttpSetRecvBlockSize on the connection
    int rc_req = 0;          // ... and on the request
    const char* stop = "";   // time, cap, eof or error
    std::string error;       // empty = none
};

int64_t micros(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

// All requests of a variant start reading together, so the summed rate is a
// rate over one shared window rather than over staggered ones.
struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    int arrived = 0;
    int expected = 1;
    bool open = false;
    Clock::time_point start;

    Clock::time_point arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        if (++arrived == expected) {
            start = Clock::now();
            open = true;
            condition.notify_all();
        } else {
            condition.wait(lock, [this] { return open; });
        }
        return start;
    }
};

struct Shared {
    const Knobs* knobs = nullptr;
    Context* context = nullptr;
    std::string url;
    double seconds = 20.0;
    std::mutex setup;  // sceHttp creation/send run one at a time, like the app's
    Gate gate;
};

bool fail_call(Transfer& transfer, const char* call, int code) {
    transfer.error = std::string(call) + " " + hex_code(code);
    return false;
}

// Connection, request, Range header, send and status, in the order and with the
// timeouts platform/ps5/net/ps5_http_fetcher.cpp open_and_send() uses; the one
// addition is the receive block on the connection and request for B and C.
bool open_ranged_get(Shared& shared, Transfer& transfer, Req& req) {
    const std::string& url = shared.url;
    const int connection = sceHttpCreateConnectionWithURL(shared.context->tmpl, url.c_str(), 0);
    if (connection < 0) return fail_call(transfer, "sceHttpCreateConnectionWithURL", connection);
    req.connection = connection;
    set_timeouts(connection);
    if (shared.context->block != 0) {
        transfer.rc_conn = sceHttpSetRecvBlockSize(connection, shared.context->block);
    }

    const int created = sceHttpCreateRequestWithURL(connection, kMethodGet, url.c_str(), 0);
    if (created < 0) return fail_call(transfer, "sceHttpCreateRequestWithURL", created);
    req.request = created;
    set_timeouts(created);
    if (shared.context->block != 0) {
        transfer.rc_req = sceHttpSetRecvBlockSize(created, shared.context->block);
    }

    const std::string range = splat::ps5::range_header(transfer.offset, 0);
    const int header = sceHttpAddRequestHeader(created, "Range", range.c_str(), 0);
    if (header < 0) return fail_call(transfer, "sceHttpAddRequestHeader", header);
    const int sent = sceHttpSendRequest(created, nullptr, 0);
    if (sent < 0) return fail_call(transfer, "sceHttpSendRequest", sent);

    int status = 0;
    if (sceHttpGetStatusCode(created, &status) != 0) {
        transfer.error = "cannot read the status";
        return false;
    }
    transfer.status = status;
    splat::ps5::ResponseHeaders headers;
    char* raw = nullptr;
    size_t raw_size = 0;
    if (sceHttpGetAllResponseHeaders(created, &raw, &raw_size) == 0 && raw != nullptr) {
        headers = splat::ps5::parse_response_headers(raw, raw_size);
    }
    if (headers.status == 0) headers.status = status;
    // 206 for the requested offset is the point; a 200 would restart at byte 0
    // and measure the wrong range, so it is reported as an error too.
    if (status != 206 || splat::ps5::decide_range(transfer.offset, headers) !=
                             splat::ps5::RangeDecision::append) {
        transfer.error = "HTTP " + std::to_string(status) + " for " + range;
        return false;
    }
    return true;
}

void run_transfer(Shared& shared, Transfer& transfer) {
    const Knobs& knobs = *shared.knobs;
    Req req;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(shared.setup);
        const Clock::time_point began = Clock::now();
        ok = open_ranged_get(shared, transfer, req);
        transfer.connect_us = micros(began, Clock::now());
    }
    const Clock::time_point start = shared.gate.arrive_and_wait();
    if (!ok) {
        transfer.stop = "error";
        return;
    }

    std::FILE* file = std::fopen(transfer.path.c_str(), "wb");
    if (file == nullptr) {
        transfer.error = "cannot open " + transfer.path + " for writing";
        transfer.stop = "error";
        return;
    }
    std::vector<unsigned char> chunk(knobs.chunk);
    const Clock::time_point deadline =
        start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(shared.seconds));
    for (;;) {
        const Clock::time_point before = Clock::now();
        if (before >= deadline) {
            transfer.stop = "time";
            break;
        }
        if (transfer.bytes >= knobs.cap_bytes) {
            transfer.stop = "cap";
            break;
        }
        const size_t wanted = static_cast<size_t>(
            std::min<uint64_t>(knobs.chunk, knobs.cap_bytes - transfer.bytes));
        const int read = sceHttpReadData(req.request, chunk.data(), wanted);
        const Clock::time_point after_read = Clock::now();
        transfer.read_us += micros(before, after_read);
        if (read < 0) {
            transfer.error = "sceHttpReadData " + hex_code(read);
            transfer.stop = "error";
            break;
        }
        if (read == 0) {
            transfer.stop = "eof";
            break;
        }
        const size_t got = static_cast<size_t>(read);
        ++transfer.reads;
        if (std::fwrite(chunk.data(), 1, got, file) != got) {
            transfer.error = "fwrite failed on " + transfer.path;
            transfer.stop = "error";
            break;
        }
        transfer.write_us += micros(after_read, Clock::now());
        transfer.bytes += got;
    }
    const Clock::time_point flush_began = Clock::now();
    std::fflush(file);
    std::fclose(file);
    const Clock::time_point end = Clock::now();
    transfer.write_us += micros(flush_began, end);
    transfer.body_us = micros(start, end);

    // Dropping a request mid-body is what a cancelled download does; how long
    // that takes is part of the answer.
    const Clock::time_point close_began = Clock::now();
    req.reset();
    transfer.close_us = micros(close_began, Clock::now());
}

double rate_mbps(uint64_t bytes, int64_t microseconds) {
    return microseconds > 0 ? static_cast<double>(bytes) / static_cast<double>(microseconds) : 0.0;
}

// Runs one variant: a fresh context, knobs.threads requests on their own ranges,
// one result line (and a detail line per thread when there are several), then the
// files are deleted. True when no request failed.
bool run_variant(const Knobs& knobs, const std::string& url, uint64_t base_offset, double seconds,
                 unsigned int forced_block, Watchdog& watchdog) {
    watchdog.arm(seconds + 120.0, knobs.watch_label);
    Context context = make_context(knobs, forced_block);
    if (!context.error.empty()) {
        close_context(context);
        watchdog.disarm();
        log_line("%s: result variant=%s threads=%d bytes=0 seconds=0.00 mbps=0.00 error=%s", kTag,
                 knobs.name, knobs.threads, context.error.c_str());
        return false;
    }

    Shared shared;
    shared.knobs = &knobs;
    shared.context = &context;
    shared.url = url;
    shared.seconds = seconds;
    shared.gate.expected = knobs.threads;

    std::vector<Transfer> transfers(static_cast<size_t>(knobs.threads));
    for (int index = 0; index < knobs.threads; ++index) {
        Transfer& transfer = transfers[static_cast<size_t>(index)];
        transfer.offset = base_offset + static_cast<uint64_t>(knobs.first_slot + index) * kSlotStride;
        std::string name = "http_probe.bin";
        if (knobs.threads > 1) name += ".c" + std::to_string(index);
        transfer.path = splat::bench::bench_path(name);
    }

    if (knobs.threads == 1) {
        run_transfer(shared, transfers[0]);
    } else {
        std::vector<std::thread> threads;
        for (Transfer& transfer : transfers) {
            threads.emplace_back([&shared, &transfer] { run_transfer(shared, transfer); });
        }
        for (std::thread& thread : threads) thread.join();
    }

    uint64_t bytes = 0;
    uint64_t reads = 0;
    int64_t wall_us = 0;
    int64_t read_us = 0;
    int64_t write_us = 0;
    int64_t connect_us = 0;
    int64_t close_us = 0;
    const char* stop = transfers[0].stop;
    std::string error;
    for (const Transfer& transfer : transfers) {
        bytes += transfer.bytes;
        reads += transfer.reads;
        wall_us = std::max(wall_us, transfer.body_us);
        read_us += transfer.read_us;
        write_us += transfer.write_us;
        connect_us = std::max(connect_us, transfer.connect_us);
        close_us = std::max(close_us, transfer.close_us);
        if (std::string(transfer.stop) != stop) stop = "mixed";
        if (error.empty()) error = transfer.error;
        std::remove(transfer.path.c_str());
    }

    const Transfer& first = transfers[0];
    log_line("%s: result variant=%s threads=%d bytes=%llu seconds=%.2f mbps=%.2f read_us=%lld "
             "write_us=%lld reads=%llu avg_read=%llu connect_ms=%lld close_ms=%lld status=%ld "
             "block=%u block_rc=%s/%s stop=%s error=%s",
             kTag, knobs.name, knobs.threads, static_cast<unsigned long long>(bytes),
             static_cast<double>(wall_us) / 1e6, rate_mbps(bytes, wall_us),
             static_cast<long long>(read_us), static_cast<long long>(write_us),
             static_cast<unsigned long long>(reads),
             static_cast<unsigned long long>(reads > 0 ? bytes / reads : 0),
             static_cast<long long>(connect_us / 1000), static_cast<long long>(close_us / 1000),
             first.status, context.block, hex_code(first.rc_conn).c_str(),
             hex_code(first.rc_req).c_str(), stop, error.empty() ? "none" : error.c_str());
    if (knobs.threads > 1) {
        for (size_t index = 0; index < transfers.size(); ++index) {
            const Transfer& transfer = transfers[index];
            log_line("%s: detail variant=%s thread=%zu offset=%llu bytes=%llu seconds=%.2f mbps=%.2f "
                     "read_us=%lld write_us=%lld connect_ms=%lld close_ms=%lld status=%ld stop=%s error=%s",
                     kTag, knobs.name, index, static_cast<unsigned long long>(transfer.offset),
                     static_cast<unsigned long long>(transfer.bytes),
                     static_cast<double>(transfer.body_us) / 1e6,
                     rate_mbps(transfer.bytes, transfer.body_us),
                     static_cast<long long>(transfer.read_us), static_cast<long long>(transfer.write_us),
                     static_cast<long long>(transfer.connect_us / 1000),
                     static_cast<long long>(transfer.close_us / 1000), transfer.status, transfer.stop,
                     transfer.error.empty() ? "none" : transfer.error.c_str());
        }
    }

    close_context(context);
    watchdog.disarm();
    return error.empty();
}

int run_speed(Watchdog& watchdog, const std::string& variant) {
    const std::string url = env_value("HTTP_URL", kSharpUrl);
    const uint64_t offset = std::strtoull(env_value("HTTP_OFFSET", "1000000000").c_str(), nullptr, 10);
    const double seconds =
        std::min(120.0, std::max(1.0, std::strtod(env_value("HTTP_SECONDS", "20").c_str(), nullptr)));
    const unsigned int forced_block =
        static_cast<unsigned int>(std::strtoul(env_value("HTTP_BLOCK", "0").c_str(), nullptr, 10));
    log_line("%s: speed url %s", kTag, url.c_str());
    log_line("%s: speed variant %s offset %llu seconds %.0f block %u", kTag, variant.c_str(),
             static_cast<unsigned long long>(offset), seconds, forced_block);

    // The same HEAD the model manager does first; it also tells how far the
    // ranges may reach.
    Ps5HttpFetcher fetcher;
    watchdog.arm(120.0, "head");
    const Result<HttpResource> head = fetcher.head(url);
    watchdog.disarm();
    if (!head.ok()) return fail("head", head.message());
    const uint64_t length = head.value().content_length;
    log_line("%s: head %ld length %llu ranges %d", kTag, fetcher.last_status(),
             static_cast<unsigned long long>(length), head.value().range_supported ? 1 : 0);
    if (length != 0 && offset + kSlotStride * kSlotCount > length) {
        return fail("offset", "HTTP_OFFSET " + std::to_string(offset) + " leaves less than " +
                                  std::to_string(kSlotStride * kSlotCount / kMB) + " MB of the " +
                                  std::to_string(length) + " byte file");
    }

    bool all_ok = true;
    for (const Knobs* knobs : {&kKnobsA, &kKnobsB, &kKnobsC}) {
        if (variant != "ALL" && variant != knobs->name) continue;
        if (!run_variant(*knobs, url, offset, seconds, forced_block, watchdog)) all_ok = false;
    }
    if (!all_ok) return fail("variant", "a request failed, see the result lines above");
    log_line("%s: PASS", kTag);
    return 0;
}

// ---- smoke mode (HTTP_VARIANT=smoke) --------------------------------------------

int run_smoke(Watchdog& watchdog) {
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

}  // namespace

int http_probe_run(int, char**) {
    Watchdog watchdog(kTag);
    splat::bench::apply_bench_radv_defaults(kTag);
    splat::bench::load_env_file(splat::bench::bench_path("bench.env"), kTag);

    const std::string variant = upper_ascii(env_value("HTTP_VARIANT", "all"));
    if (variant == "SMOKE") return run_smoke(watchdog);
    if (variant == "ALL" || variant == "A" || variant == "B" || variant == "C") {
        return run_speed(watchdog, variant);
    }
    return fail("HTTP_VARIANT", "'" + variant + "' is not A, B, C, all or smoke");
}
