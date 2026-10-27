// sceHttp implementation of IHttpFetcher. See ps5_http_fetcher.h for scope.
//
// Everything sce-specific is in this file. The 200-vs-206 decision and the
// header parsing live in http_range.h so the Mac test can check them.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(PS5_TITLE)

#include "platform/ps5/net/ps5_http_fetcher.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/ps5/net/http_range.h"
#include "platform/ps5/net/sce_http.h"

namespace splat {
namespace {

using ps5::decide_range;
using ps5::parse_response_headers;
using ps5::range_header;
using ps5::RangeDecision;
using ps5::ResponseHeaders;

// sceHttpCreateRequestWithURL takes the method as an enum; GET is 0 and HEAD is
// 2 (the order in Sony's SceHttpMethods). ProsperoMgr and elfldr both send
// method 0 for GET.
constexpr int kMethodGet = 0;
constexpr int kMethodHead = 2;

constexpr size_t kChunkBytes = 64 * 1024;
// 15 s to connect, 30 s per receive, as the brief asks; resolve and send get
// the same bounds so no stage can wait forever.
constexpr unsigned int kConnectTimeoutUs = 15u * 1000u * 1000u;
constexpr unsigned int kReceiveTimeoutUs = 30u * 1000u * 1000u;

std::string hex_code(int code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned int>(code));
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
    return context;
}

NetContext& shared_context() {
    // Function-local static: constructed once, never destroyed (a title does
    // not return through the CRT; its teardown is LoadExec).
    static NetContext context = make_context();
    return context;
}

std::mutex& transfer_mutex() {
    // sceHttp's template/connection state is not documented as reentrant, so
    // one transfer runs at a time per process, as ProsperoMgr serialises them.
    static std::mutex mutex;
    return mutex;
}

struct Request {
    int connection = -1;
    int request = -1;

    Request() = default;
    ~Request() { reset(); }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    void reset() {
        if (request >= 0) sceHttpDeleteRequest(request);
        if (connection >= 0) sceHttpDeleteConnection(connection);
        request = -1;
        connection = -1;
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
        request.request = created;
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

struct FileCloser {
    void operator()(std::FILE* file) const {
        if (file != nullptr) std::fclose(file);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace

Result<HttpResource> Ps5HttpFetcher::head(const std::string& url) {
    std::lock_guard<std::mutex> lock(transfer_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Result<HttpResource>::failure(context.error);

    Request request;
    const SendOutcome sent = open_and_send(context, request, url, kMethodHead, nullptr);
    if (!sent.ok) return Result<HttpResource>::failure(request_error(url, sent));

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        return Result<HttpResource>::failure("cannot read the status for " + url);
    }
    last_status_ = status;
    if (status < 200 || status >= 300) {
        return Result<HttpResource>::failure(http_error(url, status));
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
    return Result<HttpResource>::success(resource);
}

Status Ps5HttpFetcher::download(const FetchRequest& request) {
    return download_impl(request.url, request.destination, request.offset, 0, request.total_bytes,
                         request.progress, request.cancelled);
}

Status Ps5HttpFetcher::download_range(const std::string& url, const Path& destination, uint64_t offset,
                                      uint64_t max_bytes, Progress* progress,
                                      std::function<bool()> cancelled) {
    return download_impl(url, destination, offset, max_bytes, 0, progress, cancelled);
}

Status Ps5HttpFetcher::download_impl(const std::string& url, const Path& destination, uint64_t offset,
                                     uint64_t max_bytes, uint64_t total_bytes, Progress* progress,
                                     const std::function<bool()>& cancelled) {
    if (url.empty()) return Status::error("download needs a URL");
    if (destination.empty()) return Status::error("download needs a destination path");

    const Path parent = destination.parent_path();
    if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            return Status::error("cannot create " + parent.string() + ": " +
                                 directory_error.message());
        }
    }

    std::lock_guard<std::mutex> lock(transfer_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Status::error(context.error);

    Request request;
    // The probe's bounded range is what makes the first request a 206; a
    // resume asks from the partial file's offset. A plain first download sends
    // no Range header at all, like the desktop fetcher.
    std::string range;
    if (offset > 0 || max_bytes != 0) range = range_header(offset, max_bytes);
    const SendOutcome sent = open_and_send(context, request, url, kMethodGet,
                                           range.empty() ? nullptr : &range);
    if (!sent.ok) return Status::error(request_error(url, sent));

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        return Status::error("cannot read the status for " + url);
    }
    last_status_ = status;

    const ResponseHeaders headers = response_headers(request.request, status);
    const RangeDecision decision = decide_range(offset, headers);
    if (decision == RangeDecision::reject) {
        if (status != 200 && status != 206) return Status::error(http_error(url, status));
        return Status::error(url + ": the server resumed at the wrong byte offset");
    }

    uint64_t response_length = 0;
    bool have_response_length = response_content_length(request.request, &response_length);
    if (!have_response_length && headers.has_content_length) {
        response_length = headers.content_length;
        have_response_length = true;
    }

    FilePtr file(std::fopen(destination.string().c_str(), offset > 0 ? "ab" : "wb"));
    if (file == nullptr) return Status::error("cannot open " + destination.string() + " for writing");

    uint64_t effective_offset = offset;
    if (decision == RangeDecision::restart && effective_offset > 0) {
        // 200 for a ranged request: the whole body starts at byte 0, so the
        // prefix already in the file must go before the first write.
        file.reset(std::fopen(destination.string().c_str(), "wb"));
        if (file == nullptr) return Status::error("cannot reopen " + destination.string());
        effective_offset = 0;
    }

    std::vector<unsigned char> chunk(kChunkBytes);
    uint64_t written = 0;
    int last_percent = -1;
    for (;;) {
        if (cancelled && cancelled()) return Status::error("download cancelled");

        const size_t wanted = max_bytes != 0 && written < max_bytes
                                  ? static_cast<size_t>(std::min<uint64_t>(kChunkBytes, max_bytes - written))
                                  : kChunkBytes;
        const int read = sceHttpReadData(request.request, chunk.data(), wanted);
        if (read < 0) {
            return Status::error("sceHttpReadData failed for " + url + " (" + hex_code(read) +
                                 ")");
        }
        if (read == 0) break;  // clean EOF
        const size_t bytes = static_cast<size_t>(read);
        if (std::fwrite(chunk.data(), 1, bytes, file.get()) != bytes) {
            return Status::error("failed while writing " + destination.string());
        }
        written += bytes;

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
    }

    if (std::fflush(file.get()) != 0) {
        return Status::error("cannot flush " + destination.string());
    }
    if (max_bytes == 0 && have_response_length && written != response_length) {
        return Status::error(url + ": short read, got " + std::to_string(written) + " of " +
                             std::to_string(response_length) + " bytes");
    }
    if (progress != nullptr && total_bytes > 0) {
        const uint64_t done = effective_offset + written;
        progress->report(static_cast<float>(static_cast<double>(done) /
                                            static_cast<double>(total_bytes)),
                         "downloading");
    }
    return Status::success();
}

Result<std::string> Ps5HttpFetcher::get_text(const std::string& url, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(transfer_mutex());
    NetContext& context = shared_context();
    if (!context.error.empty()) return Result<std::string>::failure(context.error);

    Request request;
    const SendOutcome sent = open_and_send(context, request, url, kMethodGet, nullptr);
    if (!sent.ok) return Result<std::string>::failure(request_error(url, sent));

    int status = 0;
    if (sceHttpGetStatusCode(request.request, &status) != 0) {
        return Result<std::string>::failure("cannot read the status for " + url);
    }
    last_status_ = status;
    if (status < 200 || status >= 300) return Result<std::string>::failure(http_error(url, status));

    const ResponseHeaders headers = response_headers(request.request, status);
    uint64_t declared = 0;
    bool have_declared = response_content_length(request.request, &declared);
    if (!have_declared && headers.has_content_length) {
        declared = headers.content_length;
        have_declared = true;
    }
    if (max_bytes != 0 && have_declared && declared > max_bytes) {
        return Result<std::string>::failure(url + ": response is larger than " +
                                            std::to_string(max_bytes) + " bytes");
    }

    std::string text;
    if (have_declared && declared > 0 && declared <= max_bytes) {
        text.reserve(static_cast<size_t>(declared));
    }
    std::vector<char> chunk(kChunkBytes);
    for (;;) {
        const int read = sceHttpReadData(request.request, chunk.data(), chunk.size());
        if (read < 0) {
            return Result<std::string>::failure("sceHttpReadData failed for " + url + " (" +
                                                hex_code(read) + ")");
        }
        if (read == 0) break;
        const size_t bytes = static_cast<size_t>(read);
        if (max_bytes != 0 && text.size() + bytes > max_bytes) {
            return Result<std::string>::failure(url + ": response is larger than " +
                                                std::to_string(max_bytes) + " bytes");
        }
        text.append(chunk.data(), bytes);
    }
    return Result<std::string>::success(std::move(text));
}

}  // namespace splat

#endif  // PS5_TITLE
