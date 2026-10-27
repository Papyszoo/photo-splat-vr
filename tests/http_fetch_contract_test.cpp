// Host-only test for the 200-vs-206 contract of the PS5 HTTP fetcher.
//
// platform/ps5/net/ps5_http_fetcher.cpp is a title (PS5_TITLE) translation
// unit full of sceHttp calls and cannot run on the Mac; the decision it makes
// once a response arrives lives in platform/ps5/net/http_range.h and is tested
// here. A fake IHttpFetcher applies the same helper to a real file, so the
// resume/restart behaviour is checked through the interface, not just the
// helper's return value. No network.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "../platform/ps5/net/http_range.h"
#include "model/http_fetch.h"
#include "test_util.h"

using splat::FetchRequest;
using splat::HttpResource;
using splat::IHttpFetcher;
using splat::ps5::decide_range;
using splat::ps5::parse_response_headers;
using splat::ps5::range_header;
using splat::ps5::RangeDecision;
using splat::ps5::ResponseHeaders;
using splat::Result;
using splat::Status;

namespace {

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A server that honours Range, ignores it (200), or answers 206 for the wrong
// start offset. The download path is the one the PS5 fetcher uses: decide from
// the headers, then append, truncate or reject.
class FakeHttpFetcher final : public IHttpFetcher {
public:
    enum class Server { honors_range, ignores_range, wrong_start };

    FakeHttpFetcher(std::string payload, Server server)
        : payload_(std::move(payload)), server_(server) {}

    Result<HttpResource> head(const std::string&) override {
        HttpResource resource;
        resource.content_length = payload_.size();
        resource.range_supported = server_ == Server::honors_range;
        return Result<HttpResource>::success(resource);
    }

    Status download(const FetchRequest& request) override {
        ResponseHeaders headers;
        if (server_ == Server::ignores_range || request.offset == 0) {
            headers.status = 200;
        } else {
            headers.status = 206;
            headers.has_content_range = true;
            headers.content_range_start = server_ == Server::wrong_start ? 0 : request.offset;
            headers.content_range_total = payload_.size();
        }

        const RangeDecision decision = decide_range(request.offset, headers);
        if (decision == RangeDecision::reject) {
            return Status::error("fake: the server resumed at the wrong byte offset");
        }

        std::ofstream file(request.destination,
                           (request.offset > 0 && decision == RangeDecision::append)
                               ? (std::ios::binary | std::ios::app)
                               : (std::ios::binary | std::ios::trunc));
        if (!file) return Status::error("fake: cannot open the destination");
        const size_t start = decision == RangeDecision::restart ? 0 : static_cast<size_t>(request.offset);
        file.write(payload_.data() + start, static_cast<std::streamsize>(payload_.size() - start));
        return Status::success();
    }

    Result<std::string> get_text(const std::string&, size_t) override {
        return Result<std::string>::success(std::string("LICENCE"));
    }

private:
    std::string payload_;
    Server server_;
};

}  // namespace

TEST_CASE("http_contract: a 206 header block yields length, ranges and the range start") {
    const std::string block =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Range: bytes 1048576-2097151/99173660\r\n"
        "Content-Length: 1048576\r\n"
        "\r\n";
    const ResponseHeaders headers = parse_response_headers(block.data(), block.size());
    CHECK(headers.status == 206);
    CHECK(headers.accept_ranges);
    CHECK(headers.has_content_range);
    CHECK(headers.content_range_start == 1048576ULL);
    CHECK(headers.content_range_total == 99173660ULL);
    CHECK(headers.has_content_length);
    CHECK(headers.content_length == 1048576ULL);
}

TEST_CASE("http_contract: a redirect chain keeps the last status and its headers") {
    const std::string block =
        "HTTP/1.1 302 Found\r\n"
        "Location: https://cdn.example/file\r\n"
        "Accept-Ranges: bytes\r\n"
        "\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 99\r\n"
        "\r\n";
    const ResponseHeaders headers = parse_response_headers(block.data(), block.size());
    CHECK(headers.status == 200);
    // The final response did not advertise ranges; the redirect's header is gone.
    CHECK_FALSE(headers.accept_ranges);
    CHECK(headers.status == 200);
}

TEST_CASE("http_contract: an absent or wildcard Content-Range total does not break parsing") {
    const std::string block =
        "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 10-19/*\r\n\r\n";
    const ResponseHeaders headers = parse_response_headers(block.data(), block.size());
    CHECK(headers.status == 206);
    CHECK(headers.has_content_range);
    CHECK(headers.content_range_start == 10ULL);
    CHECK(headers.content_range_total == 0ULL);
}

TEST_CASE("http_contract: range headers are open-ended for a resume and bounded for the probe") {
    CHECK(range_header(1234, 0) == "bytes=1234-");
    CHECK(range_header(0, 1048576) == "bytes=0-1048575");
    CHECK(range_header(4096, 4096) == "bytes=4096-8191");
}

TEST_CASE("http_contract: a 200 restarts, a matching 206 appends, anything else is rejected") {
    ResponseHeaders ok;
    ok.status = 200;
    CHECK(decide_range(0, ok) == RangeDecision::restart);
    CHECK(decide_range(500, ok) == RangeDecision::restart);

    ResponseHeaders partial;
    partial.status = 206;
    partial.has_content_range = true;
    partial.content_range_start = 500;
    CHECK(decide_range(500, partial) == RangeDecision::append);
    // Offset 0 has no prefix to protect, so a 206 is still an append.
    CHECK(decide_range(0, partial) == RangeDecision::append);

    partial.content_range_start = 0;
    CHECK(decide_range(500, partial) == RangeDecision::reject);

    ResponseHeaders missing;
    missing.status = 404;
    CHECK(decide_range(0, missing) == RangeDecision::reject);
    missing.status = 0;
    CHECK(decide_range(0, missing) == RangeDecision::reject);
}

TEST_CASE("http_contract: the fake fetcher resumes, restarts on 200, and rejects a wrong offset") {
    const std::string payload = "0123456789abcdefghij";

    splat_test::TempDir resume_dir("http-contract-resume");
    {
        FakeHttpFetcher fetcher(payload, FakeHttpFetcher::Server::honors_range);
        const std::filesystem::path path = resume_dir.path() / "part.bin";
        write_bytes(path, payload.substr(0, 8));
        FetchRequest request;
        request.url = "https://example/file.bin";
        request.destination = path;
        request.offset = 8;
        const Status status = fetcher.download(request);
        REQUIRE(status.ok());
        CHECK(read_bytes(path) == payload);
        CHECK(fetcher.head(request.url).value().range_supported);
    }

    splat_test::TempDir restart_dir("http-contract-restart");
    {
        FakeHttpFetcher fetcher(payload, FakeHttpFetcher::Server::ignores_range);
        const std::filesystem::path path = restart_dir.path() / "part.bin";
        write_bytes(path, payload.substr(0, 8));
        FetchRequest request;
        request.url = "https://example/file.bin";
        request.destination = path;
        request.offset = 8;
        const Status status = fetcher.download(request);
        REQUIRE(status.ok());
        // The server ignored Range: the file starts over instead of keeping a
        // prefix followed by a second copy.
        CHECK(read_bytes(path) == payload);
    }

    splat_test::TempDir wrong_dir("http-contract-wrong");
    {
        FakeHttpFetcher fetcher(payload, FakeHttpFetcher::Server::wrong_start);
        const std::filesystem::path path = wrong_dir.path() / "part.bin";
        const std::string partial = payload.substr(0, 8);
        write_bytes(path, partial);
        FetchRequest request;
        request.url = "https://example/file.bin";
        request.destination = path;
        request.offset = 8;
        const Status status = fetcher.download(request);
        CHECK_FALSE(status.ok());
        CHECK(status.message().find("wrong byte offset") != std::string::npos);
        // A rejected response must not disturb the partial file.
        CHECK(read_bytes(path) == partial);
    }
}
