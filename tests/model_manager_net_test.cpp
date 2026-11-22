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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "model/model_manager.h"
#include "model/segmented_download.h"
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
