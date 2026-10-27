#pragma once

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
};

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
