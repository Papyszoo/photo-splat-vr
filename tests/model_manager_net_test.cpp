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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "httplib/httplib.h"

#include "model/checkpoint.h"
#include "model/converter.h"
#include "model/model_manager.h"
#include "model/segmented_download.h"
#include "model/sha256.h"
#include "platform/desktop/curl_http_fetcher.h"
#include "test_util.h"
#include "ui/model_service.h"
#include "ui/ui_state.h"

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

TEST_CASE("model manager: the desktop curl fetcher writes one segment in place") {
    const std::string payload = make_payload(4096);
    RangeServer server(payload, true);
    splat::CurlHttpFetcher fetcher;
    splat_test::TempDir temp("curl-segment");
    const splat::Path destination = temp.path() / "file.bin.part";
    write_file(destination, std::string(4096, 'X'));  // the other segments' bytes stay as they are

    splat::FetchRequest request;
    request.url = server.url();
    request.destination = destination;
    request.offset = 1000;
    request.end = 2500;
    std::atomic<uint64_t> written{0};
    request.written = &written;

    const splat::Status status = fetcher.download(request);
    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(written.load() == 1500);
    const std::string expected = std::string(1000, 'X') + payload.substr(1000, 1500) + std::string(1596, 'X');
    CHECK(read_file(destination) == expected);  // not truncated, not appended

    // A server that ignores the range must not be allowed to restart the file.
    RangeServer ignoring(payload, false);
    request.url = ignoring.url();
    written.store(0);
    const splat::Status ignored = fetcher.download(request);
    CHECK_FALSE(ignored.ok());
    CHECK(read_file(destination) == expected);
}

TEST_CASE("model manager: four curl segments assemble the exact bytes") {
    const std::string payload = make_payload(300000);
    RangeServer server(payload, true);
    splat::CurlHttpFetcher fetcher;
    splat_test::TempDir temp("curl-segments");

    splat::SegmentedDownload job;
    job.fetcher = &fetcher;
    job.url = server.url();
    job.part = temp.path() / "downloads" / "file.bin.part";
    job.total = payload.size();
    job.segments = splat::plan_segments(0, job.total, 4);
    splat::Progress progress;
    job.progress = &progress;

    const splat::Status status = splat::download_segmented(job);
    REQUIRE_MESSAGE(status.ok(), status.message());
    CHECK(read_file(job.part) == payload);
    CHECK_FALSE(std::filesystem::exists(splat::segments_path(job.part)));
    CHECK(progress.fraction() == doctest::Approx(1.0f));
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

namespace {

// A TCP server that answers HEAD, sends a GET's response headers and then never
// sends the body - or sends only its first 1 KiB - keeping the connection open.
// That is the stall class the console saw: the request is accepted and answered,
// then nothing arrives, so the socket's 30 s receive timeout is the only bound a
// transfer has left.
class StallServer {
public:
    enum class Mode { never_send, stop_mid_body };
    static constexpr uint64_t kBodyBytes = 4096;

    explicit StallServer(Mode mode) : mode_(mode) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("cannot create the stall server socket");
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            ::close(listener_);
            throw std::runtime_error("cannot bind the stall server socket");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("cannot read the stall server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~StallServer() { stop(); }

    StallServer(const StallServer&) = delete;
    StallServer& operator=(const StallServer&) = delete;

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/model.bin";
    }

private:
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

    void serve() {
        while (!stop_.load()) {
            pollfd waiting{listener_, POLLIN, 0};
            if (::poll(&waiting, 1, 50) <= 0) continue;
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) continue;
            // A client that never sends its request must not hang the server's
            // teardown: the header read gives up after two seconds.
            timeval receive_timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                         sizeof(receive_timeout));
#if defined(SO_NOSIGPIPE)
            int on = 1;
            ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string request;
        char byte = 0;
        while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
            if (::recv(client, &byte, 1, 0) <= 0) return;
            request.push_back(byte);
            if (stop_.load()) return;
        }
        const bool head = request.rfind("HEAD ", 0) == 0;
        const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                    std::to_string(kBodyBytes) +
                                    "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
        if (!send_all(client, headers)) return;
        if (head) return;
        if (mode_ == Mode::stop_mid_body) {
            const std::string part(1024, 'x');
            if (!send_all(client, part)) return;
        }
        // Keep the response open with no further body: the client waits for bytes
        // that never come. Notice the client closing, or the test ending.
        while (!stop_.load()) {
            pollfd waiting{client, POLLIN, 0};
            if (::poll(&waiting, 1, 50) > 0) {
                char scratch[256];
                if (::recv(client, scratch, sizeof(scratch), 0) <= 0) return;
            }
        }
    }

    static bool send_all(int socket, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t count = ::send(socket, data.data() + sent, data.size() - sent, 0);
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    Mode mode_;
    std::atomic<bool> stop_{false};
    int listener_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

splat::ModelBackendSpec stall_spec(const std::string& url) {
    splat::ModelBackendSpec spec = splat::da2_model_spec();
    spec.source_filename = "model.bin";
    spec.source_url = url;
    spec.source_bytes = StallServer::kBodyBytes;
    // Never verified: the transfer is aborted before the file is complete.
    spec.source_sha256 = std::string(64, '0');
    return spec;
}

// Serves the first `send_bytes` of a real checkpoint body and then holds the
// connection open, like StallServer but with a body the streaming converter can
// start on. HEAD announces the complete size, as a real server does.
class PayloadStallServer {
public:
    PayloadStallServer(std::string payload, size_t send_bytes)
        : payload_(std::move(payload)), send_bytes_(send_bytes) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("cannot create the payload server socket");
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0) {
            ::close(listener_);
            throw std::runtime_error("cannot bind the payload server socket");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listener_);
            throw std::runtime_error("cannot read the payload server port");
        }
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~PayloadStallServer() { stop(); }
    PayloadStallServer(const PayloadStallServer&) = delete;
    PayloadStallServer& operator=(const PayloadStallServer&) = delete;

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/synthetic.pt";
    }

private:
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) {
            ::close(listener_);
            listener_ = -1;
        }
    }

    void serve() {
        while (!stop_.load()) {
            pollfd waiting{listener_, POLLIN, 0};
            if (::poll(&waiting, 1, 50) <= 0) continue;
            const int client = ::accept(listener_, nullptr, nullptr);
            if (client < 0) continue;
            timeval receive_timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                         sizeof(receive_timeout));
#if defined(SO_NOSIGPIPE)
            int on = 1;
            ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string request;
        char byte = 0;
        while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
            if (::recv(client, &byte, 1, 0) <= 0) return;
            request.push_back(byte);
            if (stop_.load()) return;
        }
        const bool head = request.rfind("HEAD ", 0) == 0;
        const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                    std::to_string(payload_.size()) +
                                    "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
        if (!send_all(client, headers)) return;
        if (head) return;
        const size_t bytes = std::min(send_bytes_, payload_.size());
        if (bytes > 0 && !send_all(client, payload_.substr(0, bytes))) return;
        while (!stop_.load()) {
            pollfd waiting{client, POLLIN, 0};
            if (::poll(&waiting, 1, 50) > 0) {
                char scratch[256];
                if (::recv(client, scratch, sizeof(scratch), 0) <= 0) return;
            }
        }
    }

    static bool send_all(int socket, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t count = ::send(socket, data.data() + sent, data.size() - sent, 0);
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    std::string payload_;
    size_t send_bytes_ = 0;
    std::atomic<bool> stop_{false};
    int listener_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

std::string fixture_bytes() {
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "model" / "synthetic.pt";
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open the synthetic checkpoint fixture");
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string sha256_of(const std::string& bytes) {
    return splat::Sha256::of_hex(bytes.data(), bytes.size());
}

// A "sharp" spec for the small fixture: the streaming converter only runs for
// the sharp profile, and this shape keeps the licence gate and the real 2.8 GB
// out of the test.
splat::ModelBackendSpec stream_spec(const std::string& url, const std::string& digest,
                                    uint64_t bytes) {
    splat::ModelBackendSpec spec = splat::sharp_model_spec();
    spec.source_url = url;
    spec.source_filename = "synthetic.pt";
    spec.source_bytes = bytes;
    spec.source_sha256 = digest;
    spec.license_url.clear();
    spec.license_name = "test fixture";
    return spec;
}

template <typename Predicate>
bool wait_for(Predicate predicate, double seconds = 10.0) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

bool row_state(splat::ModelService& service, const std::string& id, const char* state) {
    for (const splat::ModelView& view : service.list()) {
        if (view.id == id) return view.state == state;
    }
    return false;
}

// The real ModelManager + ModelService over the real CurlHttpFetcher, against
// the stall server: the whole chain the console runs, minus sceHttp.
struct ServiceUnderTest {
    ServiceUnderTest(const std::string& url, double stall_seconds)
        : manager(options(url)), service(manager, state, service_options(stall_seconds)) {}

    splat::ModelManagerOptions options(const std::string& url) {
        splat::ModelManagerOptions value;
        value.state_dir = temp.path();
        value.fetcher = &fetcher;
        value.specs = {stall_spec(url)};
        value.converter = [](const splat::ModelBackendSpec&, const splat::Path&,
                             const splat::Path&, const std::string&, splat::Progress&) {
            return splat::Result<splat::ConvertStats>::failure("not reached");
        };
        return value;
    }

    splat::ModelServiceOptions service_options(double stall_seconds) {
        splat::ModelServiceOptions value;
        value.state_dir = temp.path();
        value.stall_timeout_seconds = stall_seconds;
        value.log = [this](const std::string& text) {
            std::lock_guard<std::mutex> lock(log_mutex);
            lines.push_back(text);
        };
        return value;
    }

    splat_test::TempDir temp{"stall-server"};
    splat::CurlHttpFetcher fetcher;
    splat::ModelManager manager;
    splat::UiState state;
    // Declared before the service: its log sink runs during ~ModelService.
    std::mutex log_mutex;
    std::vector<std::string> lines;
    splat::ModelService service;
};

std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += '\n';
    }
    return text;
}

}  // namespace

TEST_CASE("model service: a download that never sends fails within the stall deadline") {
    StallServer server(StallServer::Mode::never_send);
    ServiceUnderTest test(server.url(), 1.0);

    REQUIRE_MESSAGE(test.service.download("da2").ok(), "start");
    REQUIRE(wait_for([&] { return row_state(test.service, "da2", "failed"); }));
    CHECK_FALSE(test.service.downloading());

    const splat::Result<splat::ModelRecord> record = test.manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == splat::ModelState::Failed);
    std::string logged;
    {
        std::lock_guard<std::mutex> lock(test.log_mutex);
        logged = join_lines(test.lines);
    }
    MESSAGE(logged);
    CHECK(logged.find("moved no byte") != std::string::npos);
    CHECK(logged.find("aborting the transfer") != std::string::npos);
    CHECK(logged.find("failed:") != std::string::npos);
}

TEST_CASE("model service: a download that stops mid-body fails within the stall deadline") {
    StallServer server(StallServer::Mode::stop_mid_body);
    ServiceUnderTest test(server.url(), 1.0);

    REQUIRE_MESSAGE(test.service.download("da2").ok(), "start");
    REQUIRE(wait_for([&] { return row_state(test.service, "da2", "failed"); }));
    CHECK_FALSE(test.service.downloading());

    const splat::Result<splat::ModelRecord> record = test.manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == splat::ModelState::Failed);
    // The partial bytes stay for the retry.
    CHECK(record.value().downloaded_bytes > 0);
    std::string logged;
    {
        std::lock_guard<std::mutex> lock(test.log_mutex);
        logged = join_lines(test.lines);
    }
    MESSAGE(logged);
    CHECK(logged.find("moved no byte") != std::string::npos);
}

TEST_CASE("model service: cancel aborts a transfer that is waiting for its body") {
    StallServer server(StallServer::Mode::never_send);
    ServiceUnderTest test(server.url(), 60.0);

    REQUIRE_MESSAGE(test.service.download("da2").ok(), "start");
    // Let the HEAD and the GET reach the server and block there.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(test.service.downloading());
    REQUIRE_MESSAGE(test.service.cancel("da2").ok(), "cancel");
    REQUIRE(wait_for([&] { return row_state(test.service, "da2", "failed"); }, 5.0));

    const splat::Result<splat::ModelRecord> record = test.manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == splat::ModelState::Failed);
    CHECK(record.value().message.find("cancel") != std::string::npos);
}

TEST_CASE("model service: bounded stop ends a stalled download before the fixture is destroyed") {
    StallServer server(StallServer::Mode::never_send);
    auto test = std::make_unique<ServiceUnderTest>(server.url(), 60.0);
    REQUIRE_MESSAGE(test->service.download("da2").ok(), "start");
    // Let the worker reach the blocked body read, then stop it the way Quit
    // does. Only a true stop may be followed by the destruction of the service,
    // the manager, the fetcher and the log sink.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // The bound is stop()'s own, so the 500 ms above is outside the measurement:
    // only the stop and the teardown it allows are timed.
    const auto stop_started = std::chrono::steady_clock::now();
    const bool stopped = test->service.stop();
    CHECK(stopped);
    if (!stopped) {
        // A thread is still inside the fetcher: leak the fixture, as the title
        // leaks the panel on Quit.
        MESSAGE("stop() returned false; leaving the fixture allocated");
        (void)test.release();
        return;
    }
    test.reset();  // ~ModelService's own stop() is true too: nothing is detached
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - stop_started).count();
    MESSAGE("stopping and destroying the service took " << seconds << " s");
    CHECK(seconds < 2.0);
}

// ---- streaming conversion (SHARP, converter fed by the network body) --------
struct StreamServiceUnderTest {
    StreamServiceUnderTest(const std::string& url, const std::string& digest, uint64_t bytes,
                           double stall_seconds)
        : manager(options(url, digest, bytes)), service(manager, state, service_options(stall_seconds)) {}

    splat::ModelManagerOptions options(const std::string& url, const std::string& digest,
                                       uint64_t bytes) {
        splat::ModelManagerOptions value;
        value.state_dir = temp.path();
        value.fetcher = &fetcher;
        value.specs = {stream_spec(url, digest, bytes)};
        value.convert_while_downloading = true;
        value.converter = [](const splat::ModelBackendSpec&, const splat::Path&, const splat::Path&,
                             const std::string&, splat::Progress&) {
            return splat::Result<splat::ConvertStats>::failure(
                "the file converter must not run on the streaming path");
        };
        return value;
    }

    splat::ModelServiceOptions service_options(double stall_seconds) {
        splat::ModelServiceOptions value;
        value.state_dir = temp.path();
        value.stall_timeout_seconds = stall_seconds;
        value.log = [this](const std::string& text) {
            std::lock_guard<std::mutex> lock(log_mutex);
            lines.push_back(text);
        };
        return value;
    }

    splat_test::TempDir temp{"stream-service"};
    splat::CurlHttpFetcher fetcher;
    splat::ModelManager manager;
    splat::UiState state;
    std::mutex log_mutex;
    std::vector<std::string> lines;
    splat::ModelService service;
};

TEST_CASE("model manager: SHARP converts while downloading, without a .pt") {
    const std::string payload = fixture_bytes();
    RangeServer server(payload, true);
    splat_test::TempDir temp("stream-full");
    const splat::Path state_dir = temp.path();

    splat::CurlHttpFetcher fetcher;
    splat::ModelManagerOptions options;
    options.state_dir = state_dir;
    options.fetcher = &fetcher;
    options.specs = {stream_spec(server.url(), sha256_of(payload), payload.size())};
    options.convert_while_downloading = true;
    options.converter = [](const splat::ModelBackendSpec&, const splat::Path&, const splat::Path&,
                           const std::string&, splat::Progress&) {
        return splat::Result<splat::ConvertStats>::failure("the file converter must not run");
    };
    std::vector<std::string> lines;
    options.log = [&lines](const std::string& text) { lines.push_back(text); };
    splat::ModelManager manager(options);

    splat::Progress progress;
    const splat::Status status = manager.fetch("sharp", progress);
    REQUIRE_MESSAGE(status.ok(), status.message());

    const splat::Result<splat::ModelRecord> record = manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Ready);
    CHECK(std::filesystem::exists(record.value().gguf_path));
    // The streamed file is byte-identical to the file converter's output.
    const splat::Path fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "model" / "synthetic.pt";
    splat::ConvertOptions file_options;
    file_options.verify_sha256 = false;
    file_options.source_sha256 = sha256_of(payload);
    splat::Progress reference_progress;
    const splat::Path reference = temp.path() / "reference.gguf";
    const auto converted =
        splat::convert_checkpoint(fixture, reference, file_options, reference_progress);
    REQUIRE_MESSAGE(converted.ok(), converted.message());
    CHECK(read_file(record.value().gguf_path) == read_file(reference));

    // No source checkpoint and no resumable part were ever written.
    CHECK_FALSE(std::filesystem::exists(manager.downloads_dir() / "synthetic.pt"));
    CHECK_FALSE(std::filesystem::exists(manager.downloads_dir() / "synthetic.pt.part"));
    CHECK_FALSE(std::filesystem::exists(manager.models_dir() / "sharp.gguf.part"));

    bool saw_stream = false;
    bool saw_ready = false;
    for (const std::string& text : lines) {
        if (text.find("convert while downloading") != std::string::npos) saw_stream = true;
        if (text.find("ready in") != std::string::npos) saw_ready = true;
    }
    CHECK(saw_stream);
    CHECK(saw_ready);
}

TEST_CASE("model service: a stalled streaming download removes the partial GGUF") {
    const std::string payload = fixture_bytes();
    // The pickle (1,592 bytes) and the start of data/0 arrive, then nothing.
    PayloadStallServer server(payload, 2600);
    StreamServiceUnderTest test(server.url(), sha256_of(payload), payload.size(), 1.0);
    REQUIRE_MESSAGE(test.service.download("sharp").ok(), "start");
    REQUIRE(wait_for([&] { return row_state(test.service, "sharp", "failed"); }));

    const splat::Result<splat::ModelRecord> record = test.manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Failed);
    CHECK(record.value().message.find("does not resume") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(test.manager.gguf_path("sharp")));
    CHECK_FALSE(std::filesystem::exists(test.manager.gguf_path("sharp").string() + ".part"));
    CHECK_FALSE(std::filesystem::exists(test.manager.downloads_dir() / "synthetic.pt"));
    CHECK_FALSE(std::filesystem::exists(test.manager.downloads_dir() / "synthetic.pt.part"));
}

TEST_CASE("model service: cancel removes a streaming download's partial GGUF") {
    const std::string payload = fixture_bytes();
    PayloadStallServer server(payload, 2600);
    // No stall abort (60 s): the cancel is what ends this transfer.
    StreamServiceUnderTest test(server.url(), sha256_of(payload), payload.size(), 60.0);
    REQUIRE_MESSAGE(test.service.download("sharp").ok(), "start");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(test.service.downloading());
    REQUIRE_MESSAGE(test.service.cancel("sharp").ok(), "cancel");
    REQUIRE(wait_for([&] { return row_state(test.service, "sharp", "failed"); }, 5.0));

    const splat::Result<splat::ModelRecord> record = test.manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Failed);
    CHECK(record.value().message.find("cancel") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(test.manager.gguf_path("sharp")));
    CHECK_FALSE(std::filesystem::exists(test.manager.gguf_path("sharp").string() + ".part"));
}

TEST_CASE("model manager: a streamed checkpoint whose digest mismatches leaves no GGUF") {
    const std::string payload = fixture_bytes();
    RangeServer server(payload, true);
    splat_test::TempDir temp("stream-hash");
    splat::CurlHttpFetcher fetcher;
    splat::ModelManagerOptions options;
    options.state_dir = temp.path();
    options.fetcher = &fetcher;
    // A wrong pin: the stream converts, the digest is computed at the end, the
    // output must be deleted and the row must fail.
    options.specs = {stream_spec(server.url(), std::string(64, '0'), payload.size())};
    options.convert_while_downloading = true;
    splat::ModelManager manager(options);

    splat::Progress progress;
    const splat::Status status = manager.fetch("sharp", progress);
    REQUIRE_FALSE(status.ok());
    CHECK(status.message().find("SHA-256 mismatch") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(manager.gguf_path("sharp")));
    CHECK_FALSE(std::filesystem::exists(manager.gguf_path("sharp").string() + ".part"));
    CHECK_FALSE(std::filesystem::exists(manager.downloads_dir() / "synthetic.pt"));

    const splat::Result<splat::ModelRecord> record = manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Failed);
    CHECK(record.value().message.find("SHA-256 mismatch") != std::string::npos);
}

// A fetcher that serves an in-memory body through the streaming sink, so a test
// can control the write cadence exactly (a local HTTP server's timing is the
// kernel's). `pause` is slept after each accepted chunk: the transfer outlasts
// the stall deadline while every gap between two chunks is shorter than it,
// which is what a slow disk under the converter looks like to the watchdog.
class FakeStreamFetcher final : public splat::IHttpFetcher {
public:
    FakeStreamFetcher(std::string body, size_t chunk, double pause)
        : body_(std::move(body)), chunk_(chunk), pause_(pause) {}

    splat::Result<splat::HttpResource> head(const std::string&) override {
        splat::HttpResource resource;
        resource.content_length = body_.size();
        return splat::Result<splat::HttpResource>::success(resource);
    }

    splat::Status download(const splat::FetchRequest& request) override {
        const int attempt = ++downloads;
        if (fail_first && attempt == 1) return splat::Status::error("injected transfer failure");
        if (!request.sink) return splat::Status::error("the streaming test needs a sink");
        size_t at = 0;
        while (at < body_.size()) {
            if (request.cancelled && request.cancelled()) {
                return splat::Status::error("download cancelled");
            }
            const size_t size = std::min(chunk_, body_.size() - at);
            if (!request.sink(body_.data() + at, size)) {
                return splat::Status::error("the sink refused the body");
            }
            at += size;
            if (request.written != nullptr) {
                request.written->store(at, std::memory_order_relaxed);
            }
            if (pause_ > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(pause_));
            }
        }
        return splat::Status::success();
    }

    splat::Result<std::string> get_text(const std::string&, size_t) override {
        return splat::Result<std::string>::failure("no licence text in this test");
    }

    std::atomic<int> downloads{0};
    bool fail_first = false;

private:
    std::string body_;
    size_t chunk_ = 1u << 20;
    double pause_ = 0.0;
};

// HEAD and every transfer fail at once: the Models free-space gate is checked
// before a worker starts, so the transfer itself only has to end quickly.
class UnreachableFetcher final : public splat::IHttpFetcher {
public:
    splat::Result<splat::HttpResource> head(const std::string&) override {
        return splat::Result<splat::HttpResource>::failure("no network in this test");
    }
    splat::Status download(const splat::FetchRequest&) override {
        return splat::Status::error("no network in this test");
    }
    splat::Result<std::string> get_text(const std::string&, size_t) override {
        return splat::Result<std::string>::failure("no network in this test");
    }
};

TEST_CASE("model service: Retry after a failed streaming download starts from zero") {
    const std::string payload = fixture_bytes();
    FakeStreamFetcher fetcher(payload, 1u << 20, 0.0);
    fetcher.fail_first = true;
    splat_test::TempDir temp("stream-retry");

    splat::ModelManagerOptions options;
    options.state_dir = temp.path();
    options.fetcher = &fetcher;
    options.specs = {stream_spec("http://127.0.0.1/model.bin", sha256_of(payload), payload.size())};
    options.convert_while_downloading = true;
    splat::ModelManager manager(options);

    splat::ModelServiceOptions service_options;
    service_options.state_dir = temp.path();
    service_options.stall_timeout_seconds = 60.0;
    splat::UiState state;
    splat::ModelService service(manager, state, service_options);

    REQUIRE_MESSAGE(service.download("sharp").ok(), "first attempt starts");
    REQUIRE(wait_for([&] { return row_state(service, "sharp", "failed"); }));
    const splat::Result<splat::ModelRecord> failed = manager.status("sharp");
    REQUIRE_MESSAGE(failed.ok(), failed.message());
    CHECK(failed.value().state == splat::ModelState::Failed);
    CHECK(failed.value().message.find("does not resume") != std::string::npos);
    // Nothing of the failed attempt is left for a resume.
    CHECK_FALSE(std::filesystem::exists(manager.gguf_path("sharp")));
    CHECK_FALSE(std::filesystem::exists(manager.gguf_path("sharp").string() + ".part"));
    CHECK_FALSE(std::filesystem::exists(manager.downloads_dir() / "synthetic.pt"));

    // A hard kill mid-stream leaves models/sharp.gguf.part and no Ready record.
    // The startup pass (ModelService's monitor loop calls this once, on its
    // worker) deletes it, so a later Delete or launch does not wait for a Retry
    // that may never come.
    const splat::Path stale_gguf_part = manager.gguf_path("sharp").string() + ".part";
    std::filesystem::create_directories(manager.models_dir());
    write_file(stale_gguf_part, "half a GGUF");
    CHECK(manager.reclaim_streamed_sources());
    CHECK_FALSE(std::filesystem::exists(stale_gguf_part));

    // Retry: the same row, from byte 0, and this time the body arrives.
    REQUIRE_MESSAGE(service.download("sharp").ok(), "Retry starts");
    REQUIRE(wait_for([&] { return row_state(service, "sharp", "ready"); }, 20.0));
    const splat::Result<splat::ModelRecord> record = manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Ready);
    CHECK(fetcher.downloads.load() == 2);
    CHECK(std::filesystem::file_size(record.value().gguf_path) > 0);

    // The streamed file is the file converter's output, byte for byte.
    const splat::Path fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "model" / "synthetic.pt";
    splat::ConvertOptions file_options;
    file_options.verify_sha256 = false;
    file_options.source_sha256 = sha256_of(payload);
    splat::Progress progress;
    const splat::Path reference = temp.path() / "reference.gguf";
    const auto converted = splat::convert_checkpoint(fixture, reference, file_options, progress);
    REQUIRE_MESSAGE(converted.ok(), converted.message());
    CHECK(read_file(record.value().gguf_path) == read_file(reference));

    // A .pt left beside a Ready GGUF by 0.6.1 is reclaimed without a download:
    // the model service's stall watch asks for it once at startup (this call),
    // and a fetch() on the Ready record does the same.
    const splat::Path legacy = manager.downloads_dir() / "synthetic.pt";
    REQUIRE(std::filesystem::create_directories(manager.downloads_dir()));
    write_file(legacy, "legacy checkpoint bytes");
    write_file(legacy.string() + ".part", "legacy partial");
    CHECK(manager.reclaim_streamed_sources());
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK_FALSE(std::filesystem::exists(legacy.string() + ".part"));

    write_file(legacy, "legacy checkpoint bytes");
    splat::Progress again;
    REQUIRE_MESSAGE(manager.fetch("sharp", again).ok(), "fetch on a Ready row");
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(std::filesystem::exists(manager.gguf_path("sharp")));
}

TEST_CASE("model manager: an imported checkpoint is removed after its conversion") {
    // The inbox/import path runs the file converter, and the streaming backend
    // must not keep its downloads/ copy afterwards: only the GGUF stays.
    const std::string payload = fixture_bytes();
    splat_test::TempDir temp("import-cleanup");
    splat::CurlHttpFetcher fetcher;  // the import path is offline; never used
    splat::ModelManagerOptions options;
    options.state_dir = temp.path();
    options.fetcher = &fetcher;
    options.specs = {stream_spec("http://127.0.0.1/model.bin", sha256_of(payload), payload.size())};
    options.convert_while_downloading = true;
    options.converter = [](const splat::ModelBackendSpec&, const splat::Path& source,
                           const splat::Path& out, const std::string& sha,
                           splat::Progress& progress) {
        splat::ConvertOptions file_options;
        file_options.verify_sha256 = false;  // the manager checked the pin already
        file_options.source_sha256 = sha;
        return splat::convert_checkpoint(source, out, file_options, progress);
    };
    splat::ModelManager manager(options);

    const splat::Path inbox = manager.inbox_dir() / "synthetic.pt";
    REQUIRE_MESSAGE(std::filesystem::create_directories(manager.inbox_dir()), "inbox");
    write_file(inbox, payload);
    splat::Progress progress;
    const splat::Status status = manager.import_file(inbox, progress);
    REQUIRE_MESSAGE(status.ok(), status.message());

    const splat::Result<splat::ModelRecord> record = manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Ready);
    CHECK_FALSE(std::filesystem::exists(manager.downloads_dir() / "synthetic.pt"));
    CHECK(std::filesystem::exists(manager.gguf_path("sharp")));
}

TEST_CASE("model service: a slow writer that keeps reporting bytes is not a stall") {
    // Every gap between two written chunks is 0.4 s, the stall deadline is 1 s,
    // and the whole write takes ~2 s: longer than the deadline, but never still.
    const std::string payload = fixture_bytes();
    FakeStreamFetcher fetcher(payload, 2048, 0.4);
    splat_test::TempDir temp("stream-slow-writer");

    splat::ModelManagerOptions options;
    options.state_dir = temp.path();
    options.fetcher = &fetcher;
    options.specs = {stream_spec("http://127.0.0.1/model.bin", sha256_of(payload), payload.size())};
    options.convert_while_downloading = true;
    splat::ModelManager manager(options);

    splat::ModelServiceOptions service_options;
    service_options.state_dir = temp.path();
    service_options.stall_timeout_seconds = 1.0;
    std::vector<std::string> lines;
    std::mutex log_mutex;
    service_options.log = [&lines, &log_mutex](const std::string& text) {
        std::lock_guard<std::mutex> lock(log_mutex);
        lines.push_back(text);
    };
    splat::UiState state;
    splat::ModelService service(manager, state, service_options);

    REQUIRE_MESSAGE(service.download("sharp").ok(), "start");
    REQUIRE(wait_for([&] { return row_state(service, "sharp", "ready"); }, 20.0));
    const splat::Result<splat::ModelRecord> record = manager.status("sharp");
    REQUIRE_MESSAGE(record.ok(), record.message());
    CHECK(record.value().state == splat::ModelState::Ready);
    std::string logged;
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        logged = join_lines(lines);
    }
    MESSAGE(logged);
    CHECK(logged.find("moved no byte") == std::string::npos);
}

TEST_CASE("model service: the free-space gate asks for the GGUF only when streaming") {
    // The streaming path writes only the GGUF (~1.31 GB + margin); the file path
    // writes the 2.8 GB .pt first. A console with 2 GiB free must be allowed to
    // stream SHARP and refused the file download.
    struct GateService {
        GateService(bool streaming, uint64_t free_bytes)
            : manager(options(streaming)), service(manager, state, service_options(free_bytes)) {}

        splat::ModelManagerOptions options(bool streaming) {
            splat::ModelManagerOptions value;
            value.state_dir = temp.path();
            value.fetcher = &fetcher;
            value.specs = {splat::sharp_model_spec()};
            value.convert_while_downloading = streaming;
            return value;
        }

        splat::ModelServiceOptions service_options(uint64_t free_bytes) {
            splat::ModelServiceOptions value;
            value.state_dir = temp.path();
            value.free_bytes = [free_bytes](const splat::Path&) { return free_bytes; };
            value.stall_timeout_seconds = 60.0;
            return value;
        }

        splat_test::TempDir temp{"free-space"};
        UnreachableFetcher fetcher;
        splat::ModelManager manager;
        splat::UiState state;
        splat::ModelService service;
    };

    const uint64_t two_gib = 2ull << 30;
    const uint64_t one_gib = 1ull << 30;

    {
        GateService gate(true, two_gib);
        REQUIRE_MESSAGE(gate.manager.accept_license("sharp", "licence text").ok(), "licence");
        // The file path would need ~4.4 GB here; the streaming path needs ~1.6.
        const splat::Status started = gate.service.download("sharp");
        REQUIRE_MESSAGE(started.ok(), started.message());
        // It ends on the test fetcher's HEAD failure, not on the gate.
        REQUIRE(wait_for([&] { return row_state(gate.service, "sharp", "failed"); }));
        const splat::Result<splat::ModelRecord> record = gate.manager.status("sharp");
        REQUIRE_MESSAGE(record.ok(), record.message());
        CHECK(record.value().message.find("cannot reach") != std::string::npos);
    }
    {
        GateService gate(true, one_gib);
        REQUIRE_MESSAGE(gate.manager.accept_license("sharp", "licence text").ok(), "licence");
        const splat::Status refused = gate.service.download("sharp");
        CHECK_FALSE(refused.ok());
        CHECK(refused.message().find("GB free") != std::string::npos);
    }
    {
        GateService gate(false, two_gib);
        REQUIRE_MESSAGE(gate.manager.accept_license("sharp", "licence text").ok(), "licence");
        const splat::Status refused = gate.service.download("sharp");
        CHECK_FALSE(refused.ok());
        CHECK(refused.message().find("GB free") != std::string::npos);
    }
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
