// sceHttp implementation of IHttpFetcher. See ps5_http_fetcher.h for scope.
//
// Everything sce-specific is in this file. The 200-vs-206 decision and the
// header parsing live in http_range.h so the Mac test can check them.
//
// Two cross-thread concerns live here:
//  - setup_mutex() serialises create/send/status/headers/delete (exclusive) and
//    surrounds each body read (shared), so a teardown or a second request's
//    setup never runs during a read while two download bodies still run
//    together.
//  - abort() calls sceHttpAbortRequest on the live request ids from another
//    thread. A sceHttpReadData that does not return cannot be stopped by the
//    reading thread, so the model service's stall watchdog and Cancel use it.
//    The id handshake lives in live_requests.h: the registry list is never
//    locked across an SDK call, and abort() never waits for an entry whose
//    delete or earlier abort is still in flight.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(PS5_TITLE)

#include "platform/ps5/net/ps5_http_fetcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#include <system_error>
#include <utility>
#include <vector>

#include "model/io_hint.h"
#include "platform/ps5/net/http_range.h"
#include "platform/ps5/net/sce_http.h"

namespace splat {
namespace {

using ps5::decide_range;
using ps5::parse_response_headers;
using ps5::range_header;
using ps5::RangeDecision;
using ps5::ResponseHeaders;

using Clock = std::chrono::steady_clock;

// sceHttpCreateRequestWithURL takes the method as an enum; GET is 0 and HEAD is
// 2 (the order in Sony's SceHttpMethods). ProsperoMgr and elfldr both send
// method 0 for GET.
constexpr int kMethodGet = 0;
constexpr int kMethodHead = 2;

// 64 KiB per sceHttpReadData: the read size of the only full model download
// that ever completed on this console (2.8 GB at 2-3 MB/s,
// runs/v0.2-library/REPORT-060-download.md). 0.6.0 moved to 256 KiB reads plus
// sceHttpSetRecvBlockSize, which was only ever proven on bodies of 60 MB or
// less (the bench probe, runs/v0.2-library/REPORT-060-probe.md), and the two
// console runs that used it stopped on the first body read
// (runs/issue-1-crash/sharp-read-report.md). The probe also measured the 64 KiB
// read faster (3.86 vs 3.14 MB/s), so this costs nothing.
constexpr size_t kChunkBytes = 64 * 1024;
// The read stays 64 KiB (the size this console is proven with), the write is
// not: the body is collected and written in 1 MiB calls. The SHARP-ready
// investigation measured ~15-40 ms per 32-64 KiB write once the page cache was
// full (runs/issue-1-crash/sharp-slow-report.md §2.6), while the same transfer
// ran at 4 MB/s before that point; fewer, larger writes are the cheap half of
// that fix. The other half is the 64 MiB flush/fsync/drop below.
constexpr size_t kWriteChunkBytes = 1u << 20;
// 15 s to connect, 30 s per receive, as the brief asks; resolve and send get
// the same bounds so no stage can wait forever.
constexpr unsigned int kConnectTimeoutUs = 15u * 1000u * 1000u;
constexpr unsigned int kReceiveTimeoutUs = 30u * 1000u * 1000u;
// One klog line per ~5 s of body bytes: enough to date a stall on the next
// console run, far from a per-read (or per-frame) line.
constexpr std::chrono::milliseconds kByteLogInterval{5000};
// A 64 KiB read takes ~25 ms at the console's 2-3 MB/s, so a read this slow is
// an anomaly worth a number in the byte line (never a line of its own).
constexpr std::chrono::milliseconds kSlowRead{1000};

std::string hex_code(int code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned int>(code));
    return text;
}

std::string seconds_text(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

// "1.03 MB/s": the one number the next console klog needs next to the bytes.
std::string rate_text(uint64_t bytes, double seconds) {
    char text[32];
    if (seconds <= 0.0) return "? MB/s";
    std::snprintf(text, sizeof(text), "%.2f MB/s", static_cast<double>(bytes) / seconds / 1e6);
    return text;
}

// One shared net/ssl/http/template context. The reference implementation
// builds one per transfer and never calls sceNetTerm(); this keeps the same
// lifetime for the process (a title ends through LoadExec) while allowing the
// model manager to reuse it for repeated fetches.
struct NetContext {
    int net_pool = -1;
    int ssl_ctx = -1;
    int http_ctx = -1;
    int tmpl = -1;
    std::string error;
};

void set_timeouts(int id) {
    sceHttpSetResolveTimeOut(id, kConnectTimeoutUs);
    sceHttpSetConnectTimeOut(id, kConnectTimeoutUs);
    sceHttpSetSendTimeOut(id, kReceiveTimeoutUs);
    sceHttpSetRecvTimeOut(id, kReceiveTimeoutUs);
}

NetContext make_context() {
    NetContext context;
    // sceNetInit returns nonzero when the process already initialised the
    // stack, which is not an error (ProsperoMgr treats only < 0 as failure).
    if (sceNetInit() < 0) {
        context.error = "sceNetInit failed; the console network stack is unavailable";
        return context;
    }
    // The 0.5.x sizes, i.e. the ones that carried the 2.8 GB model download on
    // this console (runs/v0.2-library/REPORT-060-download.md): ProsperoMgr runs
    // 512 KiB / 384 KiB / 256 KiB as well. 0.6.0's 2 MiB pools were sized for
    // four connections at once, and the console now runs one transfer at a time.
    context.net_pool = sceNetPoolCreate("splat_http", 512 * 1024, 0);
    if (context.net_pool < 0) {
        context.error = "sceNetPoolCreate failed (" + hex_code(context.net_pool) + ")";
        return context;
    }
    context.ssl_ctx = sceSslInit(384 * 1024);
    if (context.ssl_ctx < 0) {
        context.error = "sceSslInit failed (" + hex_code(context.ssl_ctx) + ")";
        return context;
    }
    context.http_ctx = sceHttpInit(context.net_pool, context.ssl_ctx, 256 * 1024);
    if (context.http_ctx < 0) {
        context.error = "sceHttpInit failed (" + hex_code(context.http_ctx) + ")";
        return context;
    }
    context.tmpl = sceHttpCreateTemplate(context.http_ctx, "ps5-splat-vr/1.0", 2, 1);
    if (context.tmpl < 0) {
        context.error = "sceHttpCreateTemplate failed (" + hex_code(context.tmpl) + ")";
        return context;
    }
    // Content-Range and Accept-Ranges must never be truncated out of the
    // header block the contract reads.
    sceHttpSetResponseHeaderMaxSize(context.tmpl, 0x4000);
    // Redirects are followed by sceHttp itself; huggingface.co answers the
    // pinned checkpoint URL with a 302 to its CDN.
    const int redirect = sceHttpSetAutoRedirect(context.tmpl, 1);
    if (redirect < 0) {
        context.error = "sceHttpSetAutoRedirect failed (" + hex_code(redirect) + ")";
        return context;
    }
    set_timeouts(context.tmpl);
    // No sceHttpSetRecvBlockSize: sceHttp's own receive block is what moved the
    // 2.8 GB download on this console, no reference under $SPLAT_DATA_DIR/ref
    // calls it (ProsperoMgr, ShadowMountPlus, elfldr), and the probe measured no
    // gain from 262144 (3.14 MB/s against 3.86). It is the one non-default
    // receive option this fetcher had, and both console runs that used it
    // stopped on the first body read.
    return context;
}

NetContext& shared_context() {
    // Function-local static: constructed once, never destroyed (a title does
    // not return through the CRT; its teardown is LoadExec).
    static NetContext context = make_context();
    return context;
}

std::shared_mutex& setup_mutex() {
    // sceHttp's template/connection state is not documented as reentrant, so
    // creating and sending a request (through reading its status and headers) is
    // one step at a time per process, as ProsperoMgr serialises them.
    //
    // The lock has two modes, and every call takes one of them:
    //  - exclusive: create, send, status and headers, delete, and a whole
    //    licence text GET. No other transfer's create/delete can run then.
    //  - shared: each sceHttpReadData body call of a download. Download body
    //    reads still overlap each other - the console probe read four bodies
    //    together (variant C) - but a create, send or delete never runs during
    //    one. The licence text GET takes both its setup and its body
    //    exclusively (it is small), so no other request overlaps it.
    // That is the discipline the first four-way console download did not have.
    // Why it wedged there is not proven (the klog has no HEAD-to-body marker and
    // says nothing of sceHttp's internals); the leading hypothesis is that a
    // request was set up while another body read, which this lock now excludes
    // (runs/issue-1-crash/sharp-dl-verify.md section 1.3). The console also runs
    // one download transfer at a time (ModelManagerOptions::max_parallel_
    // transfers), so read windows stay uncontended.
    static std::shared_mutex mutex;
    return mutex;
}

// One sceHttpReadData with the shared side held; see setup_mutex().
int read_body(int request, void* data, size_t size) {
    std::shared_lock<std::shared_mutex> reading(setup_mutex());
    return sceHttpReadData(request, data, size);
}

struct Request {
    int connection = -1;
    int request = -1;
    // The entry another thread aborts (live_requests.h). The call's Scope keeps
    // it alive for as long as this Request exists.
    ps5::LiveRequest* live = nullptr;

    Request() = default;
    ~Request() { reset(); }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    // Publish the created id to abort() (also updates this transfer's copy).
    void publish(int id) {
        request = id;
        if (live != nullptr) ps5::LiveRegistry::publish(*live, id);
    }

    void reset() {
        if (request >= 0) {
            const int id = request;
            request = -1;
            // The owner's delete holds the entry's mutex, so an abort either
            // finishes on the still-live id first or sees -1 afterwards.
            if (live != nullptr) {
                ps5::LiveRegistry::retire(*live, [id] { sceHttpDeleteRequest(id); });
            } else {
                sceHttpDeleteRequest(id);
            }
        }
        if (connection >= 0) {
            sceHttpDeleteConnection(connection);
            connection = -1;
        }
    }
};

struct SendOutcome {
    bool ok = false;
    int code = 0;
    const char* call = "";
};

// Creates the connection + request and sends it. One retry with a fresh pair
// on a transient failure (DNS hiccup, a connection the console closed); the
// template and its SSL context are reused for the retry.
SendOutcome open_and_send(NetContext& context, Request& request, const std::string& url, int method,
                          const std::string* range) {
    SendOutcome outcome;
    for (int attempt = 0; attempt < 2; ++attempt) {
        request.reset();

        const int connection = sceHttpCreateConnectionWithURL(context.tmpl, url.c_str(), 0);
        if (connection < 0) {
            outcome = {false, connection, "sceHttpCreateConnectionWithURL"};
            continue;
        }
        request.connection = connection;
        set_timeouts(request.connection);

        const int created = sceHttpCreateRequestWithURL(request.connection, method, url.c_str(), 0);
        if (created < 0) {
            outcome = {false, created, "sceHttpCreateRequestWithURL"};
            continue;
        }
        request.publish(created);
        set_timeouts(request.request);

        if (range != nullptr) {
            const int header = sceHttpAddRequestHeader(request.request, "Range", range->c_str(), 0);
            if (header < 0) {
                outcome = {false, header, "sceHttpAddRequestHeader"};
                continue;
            }
        }

        const int sent = sceHttpSendRequest(request.request, nullptr, 0);
        if (sent < 0) {
            outcome = {false, sent, "sceHttpSendRequest"};
            continue;
        }
        outcome = {true, 0, ""};
        return outcome;
    }
    return outcome;
}

// A failed connect/send on an https URL is most often TLS, and a console whose
// clock/date is wrong rejects every certificate: say so in the message rather
// than leaving the user with an opaque negative code.
std::string request_error(const std::string& url, const SendOutcome& outcome) {
    std::string message = std::string(outcome.call) + " failed for " + url + " (" +
                          hex_code(outcome.code) + ")";
    if (url.rfind("https://", 0) == 0) {
        message += "; if TLS failed, check the console clock/date";
    }
    return message;
}

std::string http_error(const std::string& url, long status) {
    return "HTTP " + std::to_string(status) + " from " + url;
}

// True when the response carried a concrete Content-Length (type 0 in
// sceHttpGetResponseContentLength, as the reference reads it).
bool response_content_length(int request, uint64_t* length) {
    int type = 1;
    uint64_t value = 0;
    if (sceHttpGetResponseContentLength(request, &type, &value) != 0) return false;
    if (type != 0) return false;
    *length = value;
    return true;
}

ResponseHeaders response_headers(int request, long status) {
    ResponseHeaders headers;
    char* raw = nullptr;
    size_t size = 0;
    if (sceHttpGetAllResponseHeaders(request, &raw, &size) == 0 && raw != nullptr) {
        headers = parse_response_headers(raw, size);
    }
    if (headers.status == 0) headers.status = status;
    return headers;
}

// Why another thread aborted this transfer, as an error message. The abort
// reason wins over a read's own error code: the code is a symptom of the abort.
Status abort_error(const std::string& url, int reason, int code) {
    if (reason == static_cast<int>(FetchAbort::stalled) + 1) {
        return Status::error(url + ": the transfer stopped making progress and was aborted" +
                             (code < 0 ? " (" + hex_code(code) + ")" : ""));
    }
    if (reason != 0) return Status::error("download cancelled");
    return Status::error("sceHttpReadData failed for " + url + " (" + hex_code(code) + ")");
}

struct FileCloser {
    void operator()(std::FILE* file) const {
        if (file != nullptr) std::fclose(file);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace

void Ps5HttpFetcher::abort(FetchAbort reason) {
    const int code = static_cast<int>(reason) + 1;
    // The registry list is copied under its own mutex; each abort then takes
    // that entry's mutex with try_lock and holds it across sceHttpAbortRequest.
    // It therefore never waits for a delete (or another abort) that has not
    // returned, and an id is never used after its request was freed.
    const std::vector<ps5::AbortAttempt> attempts = aborts_.abort_all(
        code, [](int id) { return sceHttpAbortRequest(id); });
    if (!log) return;
    for (const ps5::AbortAttempt& attempt : attempts) {
        // One line per transfer: the watchdog and Cancel repeat the abort every
        // 250 ms while it does not take effect, which must not repeat the line.
        if (attempt.repeated) continue;
        log("model net: aborting request " + std::to_string(attempt.id) + " (" +
            (reason == FetchAbort::stalled ? "no progress" : "cancelled") + ")" +
            (attempt.code < 0 ? ", sceHttpAbortRequest " + hex_code(attempt.code) : ""));
    }
}

Result<HttpResource> Ps5HttpFetcher::head(const std::string& url) {
    std::unique_lock<std::shared_mutex> lock(setup_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Result<HttpResource>::failure(context.error);

    if (log) log("model net: HEAD " + url);
    ps5::LiveRegistry::Scope live(aborts_);
    Request request;
    request.live = &live.request();
    const SendOutcome sent = open_and_send(context, request, url, kMethodHead, nullptr);
    if (!sent.ok) {
        const int why = live.request().reason.load(std::memory_order_relaxed);
        const std::string message =
            why != 0 ? abort_error(url, why, sent.code).message() : request_error(url, sent);
        if (log) log("model net: HEAD failed: " + message);
        return Result<HttpResource>::failure(message);
    }

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        const int why = live.request().reason.load(std::memory_order_relaxed);
        const std::string message =
            why != 0 ? abort_error(url, why, 0).message() : "cannot read the status for " + url;
        if (log) log("model net: HEAD failed: " + message);
        return Result<HttpResource>::failure(message);
    }
    last_status_ = status;
    if (status < 200 || status >= 300) {
        const std::string message = http_error(url, status);
        if (log) log("model net: HEAD failed: " + message);
        return Result<HttpResource>::failure(message);
    }

    const ResponseHeaders headers = response_headers(request.request, status);
    HttpResource resource;
    uint64_t length = 0;
    if (response_content_length(request.request, &length) && length > 0) {
        resource.content_length = length;
    } else if (headers.has_content_length && headers.content_length > 0) {
        // Some HEAD responses only expose Content-Length as a header.
        resource.content_length = headers.content_length;
    }
    resource.range_supported = headers.accept_ranges;
    if (log) {
        log("model net: HEAD status " + std::to_string(status) + ", " +
            std::to_string(resource.content_length) + " bytes, ranges " +
            (resource.range_supported ? "yes" : "no"));
    }
    return Result<HttpResource>::success(resource);
}

Status Ps5HttpFetcher::download(const FetchRequest& request) {
    if (request.end != 0) {
        // One segment of a parallel download: [offset, end) written in place.
        // Bodies arrive out of order, so no hash-while-downloading here.
        if (request.end <= request.offset) return Status::error("download segment is empty");
        return download_impl(request.url, request.destination, request.offset,
                             request.end - request.offset, request.total_bytes, request.progress,
                             request.cancelled, true, request.written, nullptr);
    }
    return download_impl(request.url, request.destination, request.offset, 0, request.total_bytes,
                         request.progress, request.cancelled, false, request.written, request.hash);
}

Status Ps5HttpFetcher::download_range(const std::string& url, const Path& destination, uint64_t offset,
                                      uint64_t max_bytes, Progress* progress,
                                      std::function<bool()> cancelled) {
    return download_impl(url, destination, offset, max_bytes, 0, progress, cancelled, false, nullptr,
                         nullptr);
}

Status Ps5HttpFetcher::download_impl(const std::string& url, const Path& destination, uint64_t offset,
                                     uint64_t max_bytes, uint64_t total_bytes, Progress* progress,
                                     const std::function<bool()>& cancelled, bool in_place,
                                     std::atomic<uint64_t>* written_out, Sha256* hash) {
    if (url.empty()) return Status::error("download needs a URL");
    if (destination.empty()) return Status::error("download needs a destination path");
    // A cancel that arrived before this transfer started must not open a
    // connection at all (and `cancelled` is polled again after the setup lock).
    if (cancelled && cancelled()) return Status::error("download cancelled");

    const Path parent = destination.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return Status::error("cannot create " + parent.string() + ": " +
                                 directory_error.message());
        }
    }

    std::unique_lock<std::shared_mutex> lock(setup_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Status::error(context.error);

    ps5::LiveRegistry::Scope live(aborts_);
    Request request;
    request.live = &live.request();
    if (cancelled && cancelled()) return Status::error("download cancelled");
    // The probe's bounded range is what makes the first request a 206; a
    // resume asks from the partial file's offset. A plain first download sends
    // no Range header at all, like the desktop fetcher.
    std::string range;
    if (offset > 0 || max_bytes != 0) range = range_header(offset, max_bytes);
    if (log) {
        log((in_place ? "model net: GET segment " : "model net: GET ") + url +
            (range.empty() ? "" : " range " + range));
    }
    const SendOutcome sent = open_and_send(context, request, url, kMethodGet,
                                           range.empty() ? nullptr : &range);
    if (!sent.ok) {
        const std::string message = request_error(url, sent);
        if (log) log("model net: request failed: " + message);
        return Status::error(message);
    }
    if (log) log("model net: request " + std::to_string(request.request) + " sent");

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        const std::string message = "cannot read the status for " + url;
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    last_status_ = status;

    const ResponseHeaders headers = response_headers(request.request, status);
    const RangeDecision decision = decide_range(offset, headers);
    if (decision == RangeDecision::reject) {
        if (status != 200 && status != 206) {
            const std::string message = http_error(url, status);
            if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
            return Status::error(message);
        }
        const std::string message = url + ": the server resumed at the wrong byte offset";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }

    uint64_t response_length = 0;
    bool have_response_length = response_content_length(request.request, &response_length);
    if (!have_response_length && headers.has_content_length) {
        response_length = headers.content_length;
        have_response_length = true;
    }
    if (log) {
        log("model net: request " + std::to_string(request.request) + " status " +
            std::to_string(status) + ", body " +
            (have_response_length ? std::to_string(response_length) : std::string("unknown")) +
            " bytes");
    }

    if (in_place && (decision != RangeDecision::append || status != 206)) {
        // Restarting would erase the other segments of the file.
        const std::string message = url + ": the server ignored the byte range (HTTP " +
                                    std::to_string(status) + ")";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    if (in_place && headers.has_content_range && headers.content_range_start != offset) {
        const std::string message = url + ": the server answered a different byte range";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }

    FilePtr file(std::fopen(destination.string().c_str(),
                            in_place ? "r+b" : (offset > 0 ? "ab" : "wb")));
    if (file == nullptr) {
        const std::string message = "cannot open " + destination.string() + " for writing";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    if (in_place) {
        // Unbuffered: a byte counted in written_out is in the file, so the resume
        // state never claims more than the file holds.
        if (fseeko(file.get(), static_cast<off_t>(offset), SEEK_SET) != 0) {
            const std::string message = "cannot seek in " + destination.string();
            if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
            return Status::error(message);
        }
        std::setvbuf(file.get(), nullptr, _IONBF, 0);
    }

    uint64_t effective_offset = offset;
    if (decision == RangeDecision::restart && effective_offset > 0) {
        // 200 for a ranged request: the whole body starts at byte 0, so the
        // prefix already in the file must go before the first write.
        file.reset(std::fopen(destination.string().c_str(), "wb"));
        if (file == nullptr) {
            const std::string message = "cannot reopen " + destination.string();
            if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
            return Status::error(message);
        }
        effective_offset = 0;
        // The caller hashed the prefix that is now gone; the body starts over.
        if (hash != nullptr) hash->reset();
    }

    if (log) {
        // The body's size and the size of one read: the two numbers that decide
        // whether a first read that never returns is a size problem at all
        // (runs/issue-1-crash/sharp-read-report.md).
        log("model net: request " + std::to_string(request.request) + " body start at byte " +
            std::to_string(effective_offset) + ", " +
            (have_response_length ? std::to_string(response_length)
                                  : (max_bytes != 0 ? std::to_string(max_bytes)
                                                    : std::string("unknown"))) +
            " bytes in " + std::to_string(kChunkBytes) + "-byte reads, " +
            std::to_string(kWriteChunkBytes) + "-byte writes");
    }

    // The body reads without the setup lock (each read takes the shared side),
    // so a second transfer's setup can wait but a read never blocks the abort.
    std::vector<unsigned char> chunk(kChunkBytes);
    // 1 MiB write buffer (kWriteChunkBytes): reads stay at 64 KiB, the file sees
    // one call per MiB. `written` counts the body received; `flushed` counts the
    // bytes in the file, and that is all written_out ever reports.
    std::vector<unsigned char> write_buffer;
    write_buffer.reserve(kWriteChunkBytes);
    uint64_t written = 0;
    uint64_t flushed = 0;
    // Page-cache range not yet dropped, relative to the transfer's first byte:
    // the file range is [effective_offset + drop_from, effective_offset + flushed),
    // so a resume and a segment drop the bytes they wrote rather than the file's
    // head.
    uint64_t drop_from = 0;
    int last_percent = -1;
    uint64_t logged_bytes = 0;
    std::chrono::milliseconds slowest_read{0};
    Clock::time_point last_log = Clock::now();
    const Clock::time_point body_started = last_log;
    Status body_status = Status::success();

    // Flushes the collected bytes as one write, feeds the hash, and every 64 MiB
    // syncs them to the disk and drops them from the page cache (the §2.6 write
    // wall). False means the write failed.
    const auto flush_writes = [&]() -> bool {
        if (write_buffer.empty()) return true;
        const size_t bytes = write_buffer.size();
        if (std::fwrite(write_buffer.data(), 1, bytes, file.get()) != bytes) return false;
        if (hash != nullptr) hash->update(write_buffer.data(), bytes);
        write_buffer.clear();
        flushed += bytes;
        if (written_out != nullptr) written_out->store(flushed, std::memory_order_relaxed);
        if (flushed - drop_from >= kCacheBatchBytes) {
            // Clean pages are safe to drop; the bytes are on the disk already. A
            // failed flush or fsync fails the transfer exactly like a short
            // write: the resume position must never claim bytes that are not on
            // the disk.
            if (std::fflush(file.get()) != 0) return false;
            if (!splat::sync_and_drop(fileno(file.get()), effective_offset + drop_from,
                                      flushed - drop_from)) {
                return false;
            }
            drop_from = flushed;
        }
        return true;
    };

    lock.unlock();
    for (;;) {
        const int abort_reason = live.request().reason.load(std::memory_order_relaxed);
        if (abort_reason != 0) {
            body_status = abort_error(url, abort_reason, 0);
            break;
        }
        if (cancelled && cancelled()) {
            body_status = Status::error("download cancelled");
            break;
        }

        const size_t wanted = max_bytes != 0 && written < max_bytes
                                  ? static_cast<size_t>(std::min<uint64_t>(kChunkBytes, max_bytes - written))
                                  : kChunkBytes;
        const Clock::time_point read_began = Clock::now();
        const int read = read_body(request.request, chunk.data(), wanted);
        const std::chrono::milliseconds read_took =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - read_began);
        if (read_took > slowest_read) slowest_read = read_took;
        // Once per transfer: does a read return at all, how much, and after how
        // long. A read that never returns leaves this line missing, right after
        // the body-start line above.
        if (log && written == 0) {
            log("model net: request " + std::to_string(request.request) + " first read returned " +
                std::to_string(read) + " bytes in " + std::to_string(read_took.count()) + " ms");
        }
        if (read < 0) {
            const int why = live.request().reason.load(std::memory_order_relaxed);
            body_status = abort_error(url, why, read);
            break;
        }
        if (read == 0) break;  // clean EOF
        const size_t bytes = static_cast<size_t>(read);
        if (write_buffer.size() + bytes > kWriteChunkBytes && !flush_writes()) {
            body_status = Status::error("failed while writing " + destination.string());
            break;
        }
        write_buffer.insert(write_buffer.end(), chunk.begin(), chunk.begin() + bytes);
        written += bytes;
        if (write_buffer.size() >= kWriteChunkBytes && !flush_writes()) {
            body_status = Status::error("failed while writing " + destination.string());
            break;
        }

        if (progress != nullptr && total_bytes > 0) {
            const uint64_t done = effective_offset + written;
            const int percent = static_cast<int>((done * 1000ull) / total_bytes);
            if (percent != last_percent) {
                last_percent = percent;
                progress->report(static_cast<float>(static_cast<double>(done) /
                                                    static_cast<double>(total_bytes)),
                                 "downloading");
            }
        }
        if (max_bytes != 0 && written >= max_bytes) break;

        const Clock::time_point now = Clock::now();
        if (log && now - last_log >= kByteLogInterval) {
            const double elapsed = std::chrono::duration<double>(now - last_log).count();
            const uint64_t moved = written - logged_bytes;
            log("model net: request " + std::to_string(request.request) + " " +
                std::to_string(moved) + " bytes in " + seconds_text(elapsed) + " s (" +
                rate_text(moved, elapsed) + "), " + std::to_string(written) + " of " +
                (max_bytes != 0 ? std::to_string(max_bytes) : std::string("?")) +
                ", slowest read " + std::to_string(slowest_read.count()) + " ms" +
                (slowest_read >= kSlowRead ? " (slow)" : ""));
            last_log = now;
            logged_bytes = written;
            slowest_read = std::chrono::milliseconds{0};
        }
    }
    // Everything received is written before the transfer is judged: the resume
    // position and the bytes on disk must agree.
    if (body_status.ok() && !flush_writes()) {
        body_status = Status::error("failed while writing " + destination.string());
    }
    // Re-taking the exclusive side here is what keeps the delete below out of
    // every other transfer's read.
    lock.lock();
    if (!body_status.ok()) {
        if (log) {
            log("model net: request " + std::to_string(request.request) + " failed: " +
                body_status.message());
        }
        return body_status;
    }

    if (std::fflush(file.get()) != 0) {
        const std::string message = "cannot flush " + destination.string();
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    if (flushed > drop_from) {
        // The tail batch (under 64 MiB): sync it and drop it too, so the whole
        // file is on the disk and none of it is dirty cache. Like the batch
        // syncs, a failure fails the transfer: the file must not be verified and
        // renamed from page cache alone.
        if (!splat::sync_and_drop(fileno(file.get()), effective_offset + drop_from,
                                  flushed - drop_from)) {
            const std::string message = "cannot sync " + destination.string() + " to the disk";
            if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
            return Status::error(message);
        }
    }
    if (in_place && written != max_bytes) {
        const std::string message = url + ": short read, got " + std::to_string(written) + " of " +
                                    std::to_string(max_bytes) + " bytes";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    if (max_bytes == 0 && have_response_length && written != response_length) {
        const std::string message = url + ": short read, got " + std::to_string(written) + " of " +
                                    std::to_string(response_length) + " bytes";
        if (log) log("model net: request " + std::to_string(request.request) + " failed: " + message);
        return Status::error(message);
    }
    if (progress != nullptr && total_bytes > 0) {
        const uint64_t done = effective_offset + written;
        progress->report(static_cast<float>(static_cast<double>(done) /
                                            static_cast<double>(total_bytes)),
                         "downloading");
    }
    if (log) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - body_started).count();
        log("model net: request " + std::to_string(request.request) + " done, " +
            std::to_string(written) + " bytes in " + seconds_text(elapsed) + " s (" +
            rate_text(written, elapsed) + ")");
    }
    return Status::success();
}

Result<std::string> Ps5HttpFetcher::get_text(const std::string& url, size_t max_bytes) {
    // The whole call holds the exclusive side: a licence text is small, and its
    // create/read/delete must not overlap any other request (D2).
    std::unique_lock<std::shared_mutex> lock(setup_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Result<std::string>::failure(context.error);

    if (log) log("model net: text GET " + url);
    ps5::LiveRegistry::Scope live(aborts_);
    Request request;
    request.live = &live.request();
    const SendOutcome sent = open_and_send(context, request, url, kMethodGet, nullptr);
    if (!sent.ok) {
        const int why = live.request().reason.load(std::memory_order_relaxed);
        const std::string message =
            why != 0 ? abort_error(url, why, sent.code).message() : request_error(url, sent);
        if (log) log("model net: text GET failed: " + message);
        return Result<std::string>::failure(message);
    }

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        const int why = live.request().reason.load(std::memory_order_relaxed);
        const std::string message =
            why != 0 ? abort_error(url, why, 0).message() : "cannot read the status for " + url;
        if (log) log("model net: text GET failed: " + message);
        return Result<std::string>::failure(message);
    }
    last_status_ = status;
    if (status < 200 || status >= 300) {
        const std::string message = http_error(url, status);
        if (log) log("model net: text GET failed: " + message);
        return Result<std::string>::failure(message);
    }

    const ResponseHeaders headers = response_headers(request.request, status);
    uint64_t declared = 0;
    bool have_declared = response_content_length(request.request, &declared);
    if (!have_declared && headers.has_content_length) {
        declared = headers.content_length;
        have_declared = true;
    }
    if (max_bytes != 0 && have_declared && declared > max_bytes) {
        const std::string message = url + ": response is larger than " +
                                    std::to_string(max_bytes) + " bytes";
        if (log) log("model net: text GET failed: " + message);
        return Result<std::string>::failure(message);
    }
    if (log) {
        log("model net: text GET status " + std::to_string(status) + ", " +
            (have_declared ? std::to_string(declared) : std::string("unknown")) + " bytes");
    }

    // The body is read under the same exclusive lock (see the comment above).
    std::string text;
    if (have_declared && declared > 0 && declared <= max_bytes) {
        text.reserve(static_cast<size_t>(declared));
    }
    std::vector<char> chunk(kChunkBytes);
    uint64_t read_bytes = 0;
    Clock::time_point last_log = Clock::now();
    const Clock::time_point body_started = last_log;
    for (;;) {
        const int abort_reason = live.request().reason.load(std::memory_order_relaxed);
        if (abort_reason != 0) {
            const std::string message =
                abort_error(url, abort_reason, 0).message();
            if (log) log("model net: text GET failed: " + message);
            return Result<std::string>::failure(message);
        }
        const int read = sceHttpReadData(request.request, chunk.data(), chunk.size());
        if (read < 0) {
            const int why = live.request().reason.load(std::memory_order_relaxed);
            const std::string message = abort_error(url, why, read).message();
            if (log) log("model net: text GET failed: " + message);
            return Result<std::string>::failure(message);
        }
        if (read == 0) break;
        const size_t bytes = static_cast<size_t>(read);
        if (max_bytes != 0 && text.size() + bytes > max_bytes) {
            const std::string message = url + ": response is larger than " +
                                        std::to_string(max_bytes) + " bytes";
            if (log) log("model net: text GET failed: " + message);
            return Result<std::string>::failure(message);
        }
        text.append(chunk.data(), bytes);
        read_bytes += bytes;
        if (log && Clock::now() - last_log >= kByteLogInterval) {
            log("model net: text GET " + std::to_string(read_bytes) + " bytes so far");
            last_log = Clock::now();
        }
    }
    if (log) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - body_started).count();
        log("model net: text GET done, " + std::to_string(read_bytes) + " bytes in " +
            seconds_text(elapsed) + " s");
    }
    return Result<std::string>::success(std::move(text));
}

}  // namespace splat

#endif  // PS5_TITLE
