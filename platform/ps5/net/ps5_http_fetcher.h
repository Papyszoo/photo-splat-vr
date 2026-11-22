// PS5 implementation of the C1 network seam (core/model/http_fetch.h) on the
// console's own sceHttp/sceSsl/sceNet services.
//
// The desktop backend is libcurl (platform/desktop/curl_http_fetcher.cpp); the
// console has no libcurl, and the SDK's native stack is what ProsperoMgr
// downloads with on this hardware. The behaviour matches the desktop fetcher:
// HEAD follows redirects, a download asks for `Range: bytes=<offset>-` and
// appends, and a server that ignores the range (200 instead of 206) restarts
// the file from zero. A request with FetchRequest::end set is one segment of the
// parallel model download (core/model/segmented_download.h): a bounded range
// written in place, several at once from different threads. A create, send,
// status/header read or delete is exclusive against every body read, while two
// download body reads may run together; the licence text GET is small and holds
// the lock for its whole call, so nothing overlaps it either. See setup_mutex()
// in the .cpp.
//
// abort() is the one thing the console needs beyond the interface's polling:
// sceHttpReadData can block without honouring its receive timeout, so the model
// service's stall watchdog and Cancel call abort() from another thread, which
// calls sceHttpAbortRequest on the live request (the SDK exports it; see
// sce_http.h). abort() does nothing when no transfer is running, and it never
// waits for a request whose delete or earlier abort is still in flight
// (live_requests.h).
//
// Compiled only for the title (PS5_TITLE): the bench title links it for
// BENCH=http, and the console test measures whether a title may call sceHttp
// at all. The host-testable pieces are in http_range.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"
#include "model/http_fetch.h"
#include "platform/ps5/net/live_requests.h"

namespace splat {

class Ps5HttpFetcher final : public IHttpFetcher {
public:
    // One line per transfer phase for the klog: connect start, send, response
    // status, body start, bytes every ~5 s, end, and the error code / abort.
    // Empty (the default) logs nothing, so the bench probe is unchanged. The
    // console wires it to viewer::line in platform/ps5/viewer/ui_setup.cpp.
    std::function<void(const std::string&)> log;

    Ps5HttpFetcher() = default;
    ~Ps5HttpFetcher() override = default;

    Result<HttpResource> head(const std::string& url) override;
    Status download(const FetchRequest& request) override;
    Result<std::string> get_text(const std::string& url, size_t max_bytes) override;
    void abort(FetchAbort reason) override;

    // Probe helper (BENCH=http): a bounded ranged GET of max_bytes starting at
    // offset, appended at offset exactly like download(). The interface has no
    // length bound, and the probe wants to ask for the first 1 MiB with a Range
    // header rather than pull the whole checkpoint. max_bytes == 0 reads to EOF,
    // which is what download() does.
    Status download_range(const std::string& url, const Path& destination, uint64_t offset,
                          uint64_t max_bytes, Progress* progress,
                          std::function<bool()> cancelled);

    // Status code of the last response (0 before the first request), for the
    // probe's log line; the interface deliberately does not expose it.
    long last_status() const { return last_status_; }

private:
    Status download_impl(const std::string& url, const Path& destination, uint64_t offset,
                         uint64_t max_bytes, uint64_t total_bytes, Progress* progress,
                         const std::function<bool()>& cancelled, bool in_place,
                         std::atomic<uint64_t>* written_out, Sha256* hash);

    // The ids abort() may use. Every entry's own mutex is held across the SDK
    // call that uses its id; the registry list never is (live_requests.h).
    ps5::LiveRegistry aborts_;
    std::atomic<long> last_status_{0};
};

}  // namespace splat
