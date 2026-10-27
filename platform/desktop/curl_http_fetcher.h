#pragma once

#include "model/http_fetch.h"

namespace splat {

// Desktop HTTP fetcher on the system libcurl. The PS5 build gets its own
// implementation later; ModelManager only sees IHttpFetcher.
class CurlHttpFetcher final : public IHttpFetcher {
public:
    CurlHttpFetcher() = default;
    ~CurlHttpFetcher() override = default;

    Result<HttpResource> head(const std::string& url) override;
    Status download(const FetchRequest& request) override;
    Result<std::string> get_text(const std::string& url, size_t max_bytes) override;
};

}  // namespace splat
