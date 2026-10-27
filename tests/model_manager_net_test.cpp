// Network tests for C1, built by tools/model_cli because they need the libcurl
// fetcher. In splat_tests this file compiles to an empty translation unit (the
// macro is not defined there), so the CPU-only test binary does not gain a curl
// link dependency.
//
// The local-server cases run on every ctest invocation and exercise the real
// desktop fetcher (HEAD, ranged GET with resume, range-ignoring servers, text
// GET). The real-network case is skipped unless SPLAT_NET_TESTS=1:
//
//   SPLAT_NET_TESTS=1 ctest -R model_manager_net --output-on-failure

#include <doctest/doctest.h>

#ifdef SPLAT_MODEL_NET_TESTS

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "httplib/httplib.h"

#include "model/checkpoint.h"
#include "model/model_manager.h"
#include "platform/desktop/curl_http_fetcher.h"
#include "test_util.h"

namespace {

std::string make_payload(size_t size) {
    std::string payload(size, '\0');
    uint32_t state = 0x9e3779b9u;
    for (size_t index = 0; index < size; ++index) {
        state = state * 1664525u + 1013904223u;
        payload[index] = static_cast<char>(state >> 24);
    }
    return payload;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Local HTTP server: HEAD, Range (206/Content-Range) or, when honor_range is
// false, a plain 200 so the fetcher's restart path can be exercised.
class RangeServer {
public:
    RangeServer(std::string payload, bool honor_range)
        : payload_(std::move(payload)), honor_range_(honor_range), text_("LICENCE TEXT\n") {
        server_.Get("/file.bin", [this](const httplib::Request& request, httplib::Response& response) {
            serve(request, response);
        });
        server_.Get("/LICENSE_MODEL", [this](const httplib::Request&, httplib::Response& response) {
            response.set_content(text_, "text/plain");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("cannot bind the test HTTP server");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~RangeServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    RangeServer(const RangeServer&) = delete;
    RangeServer& operator=(const RangeServer&) = delete;

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/file.bin";
    }
    std::string license_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/LICENSE_MODEL";
    }
    const std::string& text() const { return text_; }

private:
    void serve(const httplib::Request& request, httplib::Response& response) {
        const bool head = request.method == "HEAD";
        const std::string range = request.get_header_value("Range");
        response.set_header("Accept-Ranges", honor_range_ ? "bytes" : "none");
        const bool ranged = honor_range_ && range.rfind("bytes=", 0) == 0;
        if (!ranged) {
            response.status = 200;
            if (head) {
                response.set_header("Content-Length", std::to_string(payload_.size()));
            } else {
                response.set_content(payload_, "application/octet-stream");
            }
            return;
        }
        const uint64_t start = std::strtoull(range.c_str() + 6, nullptr, 10);
        response.status = 206;
        if (head) {
            if (start < payload_.size()) {
                response.set_header("Content-Range",
                                    "bytes " + std::to_string(start) + "-" +
                                        std::to_string(payload_.size() - 1) + "/" +
                                        std::to_string(payload_.size()));
                response.set_header("Content-Length", std::to_string(payload_.size() - start));
            } else {
                response.status = 416;
            }
        } else {
            // httplib applies the Range to the body and sets Content-Range.
            response.set_content(payload_, "application/octet-stream");
        }
    }

    httplib::Server server_;
    std::string payload_;
    bool honor_range_ = true;
    std::string text_;
    int port_ = 0;
    std::thread thread_;
};

}  // namespace

TEST_CASE("model manager: the desktop curl fetcher HEAD reports size and ranges") {
    RangeServer server(make_payload(3000), true);
    splat::CurlHttpFetcher fetcher;

    const splat::Result<splat::HttpResource> head = fetcher.head(server.url());
    REQUIRE_MESSAGE(head.ok(), head.message());
    CHECK(head.value().content_length == 3000);
    CHECK(head.value().range_supported);
}

TEST_CASE("model manager: the desktop curl fetcher resumes a ranged download") {
    const std::string payload = make_payload(4096);
    RangeServer server(payload, true);
    splat::CurlHttpFetcher fetcher;
    splat_test::TempDir temp("curl-resume");
    const splat::Path destination = temp.path() / "file.bin.part";
    write_file(destination, payload.substr(0, 1000));

    splat::FetchRequest request;
    request.url = server.url();
    request.destination = destination;
    request.offset = 1000;
    request.total_bytes = payload.size();
    splat::Progress progress;
    request.progress = &progress;

    const splat::Status status = fetcher.download(request);
    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(read_file(destination) == payload);
    CHECK(progress.fraction() == doctest::Approx(1.0f));
}

TEST_CASE("model manager: the desktop curl fetcher restarts when Range is ignored") {
    const std::string payload = make_payload(2048);
    RangeServer server(payload, false);  // always answers 200 with the full body
    splat::CurlHttpFetcher fetcher;
    splat_test::TempDir temp("curl-restart");
    const splat::Path destination = temp.path() / "file.bin.part";
    write_file(destination, payload.substr(0, 700));

    splat::FetchRequest request;
    request.url = server.url();
    request.destination = destination;
    request.offset = 700;
    request.total_bytes = payload.size();

    const splat::Status status = fetcher.download(request);
    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(read_file(destination) == payload);  // truncated, not doubled
}

TEST_CASE("model manager: the desktop curl fetcher bounds a text GET") {
    RangeServer server(make_payload(16), true);
    splat::CurlHttpFetcher fetcher;

    const splat::Result<std::string> text = fetcher.get_text(server.license_url(), 1u << 20);
    REQUIRE_MESSAGE(text.ok(), text.message());
    CHECK(text.value() == server.text());

    const splat::Result<std::string> too_large = fetcher.get_text(server.license_url(), 3);
    CHECK_FALSE(too_large.ok());
}

TEST_CASE("model manager: HEAD reports the official SHARP checkpoint size") {
    const char* enabled = std::getenv("SPLAT_NET_TESTS");
    if (enabled == nullptr || std::string(enabled) != "1") {
        MESSAGE("skipped: set SPLAT_NET_TESTS=1 to run the real-network check");
        return;
    }

    const splat::ModelBackendSpec& spec = splat::sharp_model_spec();
    CHECK(spec.source_url == "https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt");

    splat::CurlHttpFetcher fetcher;
    const splat::Result<splat::HttpResource> head = fetcher.head(spec.source_url);
    REQUIRE_MESSAGE(head.ok(), head.message());
    CHECK(head.value().content_length == splat::kSharpCheckpointBytes);
    CHECK(head.value().range_supported);
}

#endif  // SPLAT_MODEL_NET_TESTS
