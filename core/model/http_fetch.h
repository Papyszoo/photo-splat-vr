#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "common/path.h"
#include "common/progress.h"
#include "common/status.h"

namespace splat {

// C1 network seam. The model manager only needs three things from the network:
// the size of a resource, a ranged GET streamed to a file, and a bounded text
// GET for the licence. Keeping the interface this small lets the PS5 backend
// arrive later (libcurl is desktop-only) without touching the state machine.
struct HttpResource {
    uint64_t content_length = 0;  // bytes, 0 when the server did not say
    bool range_supported = false; // "Accept-Ranges: bytes"
};

struct FetchRequest {
    std::string url;
    Path destination;
    // Byte offset already present in `destination`. The implementation must ask
    // for `Range: bytes=<offset>-` and append the response. A server that
    // ignores the range (200 instead of 206) is handled by truncating the file
    // and writing from the start; the manager hashes the result either way.
    uint64_t offset = 0;
    // Final size of the complete resource, used only for progress. 0 = unknown.
    uint64_t total_bytes = 0;
    // 0..1 over the whole file, resumed bytes included. May be null.
    Progress* progress = nullptr;
    // Returns true to stop the transfer. The partial file stays for a resume.
    std::function<bool()> cancelled;

    // Segment mode (end != 0): this request is one part of a download that
    // several requests run at the same time into the same file (0.6.0, see
    // core/model/segmented_download.h). It covers the bytes [offset, end):
    //  - ask for `Range: bytes=<offset>-<end - 1>`;
    //  - `destination` already exists and holds the other parts: write the body
    //    at `offset`, never truncate and never append;
    //  - a server that ignores the range (200 instead of 206) is an error,
    //    because restarting the file would erase the other segments;
    //  - the body must be exactly end - offset bytes, anything else is an error;
    //  - every byte is in the file (not in a user-space buffer) before it is
    //    added to `written`.
    uint64_t end = 0;
    // Segment mode only: bytes of this request written so far, counted from 0 at
    // `offset` and only ever increasing. The segmented download saves it as the
    // resume position, so an implementation must keep it exact. May be null.
    std::atomic<uint64_t>* written = nullptr;
};

// download() may be called from several threads at once (segment mode); head()
// and get_text() must also stay usable while a download runs.
class IHttpFetcher {
public:
    virtual ~IHttpFetcher() = default;
    IHttpFetcher(const IHttpFetcher&) = delete;
    IHttpFetcher& operator=(const IHttpFetcher&) = delete;

    // HEAD, following redirects. The manager compares content_length with the
    // pinned checkpoint size before spending bandwidth.
    virtual Result<HttpResource> head(const std::string& url) = 0;

    // Ranged GET into request.destination, appending at request.offset.
    virtual Status download(const FetchRequest& request) = 0;

    // GET into memory, refusing responses larger than max_bytes. Used for
    // LICENSE_MODEL: the licence text is never bundled with the app.
    virtual Result<std::string> get_text(const std::string& url, size_t max_bytes) = 0;

protected:
    IHttpFetcher() = default;
};

}  // namespace splat
