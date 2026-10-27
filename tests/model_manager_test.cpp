// C1 model manager tests: licence gate, ranged download with resume, hash
// verification, offline inbox, state persistence and the GGUF metadata check.
//
// The HTTP server and the fetcher in this file are test-only: the production
// libcurl fetcher lives in platform/desktop/curl_http_fetcher.cpp and needs a
// link dependency that splat_tests does not have. The local server still
// exercises the real wire protocol (HEAD, 206 with Content-Range) and the
// manager's resume logic.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib/httplib.h"

#include "model/checkpoint.h"
#include "model/da2_profile.h"
#include "model/gguf_writer.h"
#include "model/license.h"
#include "model/model_manager.h"
#include "model/sha256.h"
#include "test_util.h"

namespace {

using splat::ConvertStats;
using splat::FetchRequest;
using splat::HttpResource;
using splat::IHttpFetcher;
using splat::ModelBackendSpec;
using splat::ModelManager;
using splat::ModelManagerOptions;
using splat::ModelRecord;
using splat::ModelState;
using splat::Path;
using splat::Progress;
using splat::Result;
using splat::Sha256;
using splat::Status;

std::string make_payload(size_t size) {
    std::string payload(size, '\0');
    uint32_t state = 0x12345678u;
    for (size_t index = 0; index < size; ++index) {
        state = state * 1664525u + 1013904223u;
        payload[index] = static_cast<char>(state >> 24);
    }
    return payload;
}

std::string sha256_of(const std::string& text) { return Sha256::of_hex(text.data(), text.size()); }

void write_file(const Path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

struct UrlParts {
    std::string host;
    int port = 80;
    std::string path;
};

UrlParts parse_url(const std::string& url) {
    UrlParts parts;
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) throw std::runtime_error("test URL needs a scheme: " + url);
    const size_t slash = url.find('/', scheme + 3);
    const std::string authority = url.substr(scheme + 3, slash - scheme - 3);
    parts.path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        parts.host = authority;
    } else {
        parts.host = authority.substr(0, colon);
        parts.port = std::atoi(authority.c_str() + colon + 1);
    }
    return parts;
}

// Local HTTP server with HEAD, Range (206 + Content-Range) and the licence text.
class RangeServer {
public:
    explicit RangeServer(std::string payload)
        : payload_(std::move(payload)), license_text_("MODEL LICENCE\nresearch use only\n") {
        // httplib dispatches HEAD to the GET handlers; serve() checks the method.
        server_.Get("/model.bin", [this](const httplib::Request& request, httplib::Response& response) {
            serve(request, response);
        });
        server_.Get("/LICENSE_MODEL", [this](const httplib::Request&, httplib::Response& response) {
            response.set_content(license_text_, "text/plain");
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

    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/model.bin"; }
    std::string license_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/LICENSE_MODEL";
    }
    const std::string& license_text() const { return license_text_; }

    std::vector<uint64_t> range_starts() {
        std::lock_guard<std::mutex> lock(mutex_);
        return range_starts_;
    }

private:
    void serve(const httplib::Request& request, httplib::Response& response) {
        const bool head = request.method == "HEAD";
        const std::string range = request.get_header_value("Range");
        bool ranged = false;
        uint64_t start = 0;
        if (range.rfind("bytes=", 0) == 0) {
            start = std::strtoull(range.c_str() + 6, nullptr, 10);
            ranged = start < payload_.size();
        }
        response.set_header("Accept-Ranges", "bytes");
        if (!ranged) {
            response.status = 200;
            if (head) {
                response.set_header("Content-Length", std::to_string(payload_.size()));
            } else {
                response.set_content(payload_, "application/octet-stream");
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            range_starts_.push_back(start);
        }
        response.status = 206;
        if (head) {
            response.set_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                                                     std::to_string(payload_.size() - 1) + "/" +
                                                     std::to_string(payload_.size()));
            response.set_header("Content-Length", std::to_string(payload_.size() - start));
        } else {
            // httplib applies the request's Range to the body and fills in
            // Content-Range and Content-Length itself.
            response.set_content(payload_, "application/octet-stream");
        }
    }

    httplib::Server server_;
    std::string payload_;
    std::string license_text_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<uint64_t> range_starts_;
};

// Test fetcher: same HTTP semantics as the desktop one, minus libcurl.
class TestFetcher final : public IHttpFetcher {
public:
    // 0 = run to completion. Otherwise the transfer is aborted after this many
    // bytes have been written, which leaves a resumable .part behind.
    uint64_t abort_after = 0;
    int head_calls = 0;
    std::vector<uint64_t> range_offsets;

    Result<HttpResource> head(const std::string& url) override {
        ++head_calls;
        const UrlParts parts = parse_url(url);
        httplib::Client client(parts.host, parts.port);
        const auto response = client.Head(parts.path);
        if (!response) return Result<HttpResource>::failure(httplib::to_string(response.error()));
        if (response->status != 200) {
            return Result<HttpResource>::failure("HTTP " + std::to_string(response->status));
        }
        HttpResource resource;
        const std::string length = response->get_header_value("Content-Length");
        resource.content_length = length.empty() ? 0 : std::strtoull(length.c_str(), nullptr, 10);
        resource.range_supported =
            contains(response->get_header_value("Accept-Ranges"), "bytes");
        return Result<HttpResource>::success(resource);
    }

    Status download(const FetchRequest& request) override {
        const UrlParts parts = parse_url(request.url);
        httplib::Client client(parts.host, parts.port);
        std::error_code error;
        std::filesystem::create_directories(request.destination.parent_path(), error);

        std::FILE* file = std::fopen(request.destination.string().c_str(),
                                     request.offset > 0 ? "ab" : "wb");
        if (file == nullptr) return Status::error("cannot open the test destination");

        httplib::Headers headers;
        if (request.offset > 0) {
            headers.emplace("Range", "bytes=" + std::to_string(request.offset) + "-");
        }
        range_offsets.push_back(request.offset);

        uint64_t written = 0;
        bool aborted = false;
        const auto response = client.Get(
            parts.path, headers,
            [&](const char* data, size_t length) {
                size_t take = length;
                if (abort_after > 0 && written + take >= abort_after) {
                    take = static_cast<size_t>(abort_after - written);
                    aborted = true;
                }
                if (take > 0 && std::fwrite(data, 1, take, file) != take) {
                    aborted = true;
                    return false;
                }
                written += take;
                if (request.progress != nullptr && request.total_bytes > 0) {
                    request.progress->report(static_cast<float>(
                                                 static_cast<double>(request.offset + written) /
                                                 static_cast<double>(request.total_bytes)),
                                             "downloading");
                }
                return !aborted;
            },
            [](uint64_t, uint64_t) { return true; });
        std::fclose(file);

        if (aborted) return Status::error("interrupted");
        if (!response) return Status::error(httplib::to_string(response.error()));
        if (response->status != 200 && response->status != 206) {
            return Status::error("HTTP " + std::to_string(response->status));
        }
        return Status::success();
    }

    Result<std::string> get_text(const std::string& url, size_t max_bytes) override {
        const UrlParts parts = parse_url(url);
        httplib::Client client(parts.host, parts.port);
        const auto response = client.Get(parts.path);
        if (!response) return Result<std::string>::failure(httplib::to_string(response.error()));
        if (response->status != 200) {
            return Result<std::string>::failure("HTTP " + std::to_string(response->status));
        }
        if (response->body.size() > max_bytes) {
            return Result<std::string>::failure("response is too large");
        }
        return Result<std::string>::success(response->body);
    }
};

// Test converter: writes a tiny GGUF carrying exactly the metadata
// ModelManager checks, so the state machine can reach Ready without the real
// 2.8 GB checkpoint. The real conversion is covered by the splat_convert tests.
Result<ConvertStats> write_fake_gguf(const ModelBackendSpec& spec, const Path& out, Progress& progress,
                                     const std::string& source_sha = std::string(),
                                     uint32_t converter_version = 0) {
    splat::GgufWriter writer;
    std::vector<splat::GgufTensorDesc> tensors;
    tensors.push_back({"weight", {4}, false});
    Status status = writer.begin(out, std::move(tensors));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    writer.set_string("general.architecture", spec.gguf_architecture);
    writer.set_string(spec.name + ".backend", spec.name);
    writer.set_string(spec.name + ".source_sha256",
                      source_sha.empty() ? spec.source_sha256 : source_sha);
    writer.set_u32(spec.name + ".converter_version",
                   converter_version == 0 ? spec.converter_version : converter_version);
    status = writer.write_header();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    const float values[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    status = writer.write_tensor_bytes(values, sizeof(values));
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());
    status = writer.finish();
    if (!status.ok()) return Result<ConvertStats>::failure(status.message());

    progress.report(1.0f, "converted");
    ConvertStats stats;
    stats.tensor_count = 1;
    return Result<ConvertStats>::success(stats);
}

ModelManagerOptions::Converter fake_converter() {
    return [](const ModelBackendSpec& spec, const Path&, const Path& out, Progress& progress) {
        return write_fake_gguf(spec, out, progress);
    };
}

// A small stand-in for the pinned checkpoint: same backend identity, test URL
// and the hash/size of the local payload.
ModelBackendSpec small_spec(const std::string& name, const std::string& url,
                            const std::string& payload) {
    ModelBackendSpec spec = name == "da2" ? splat::da2_model_spec() : splat::sharp_model_spec();
    spec.source_filename = "model.bin";
    spec.source_url = url;
    spec.source_sha256 = sha256_of(payload);
    spec.source_bytes = payload.size();
    return spec;
}

ModelManagerOptions options_for(const Path& state_dir, const ModelBackendSpec& spec,
                                IHttpFetcher* fetcher) {
    ModelManagerOptions options;
    options.state_dir = state_dir;
    options.fetcher = fetcher;
    options.specs = {spec};
    options.converter = fake_converter();
    return options;
}

}  // namespace

TEST_CASE("model manager: the built-in specs pin the official checkpoints") {
    const ModelBackendSpec& sharp = splat::sharp_model_spec();
    CHECK(sharp.name == "sharp");
    CHECK(sharp.source_filename == splat::kSharpCheckpointName);
    CHECK(sharp.source_sha256 == splat::kSharpCheckpointSha256);
    CHECK(sharp.source_bytes == splat::kSharpCheckpointBytes);
    CHECK(sharp.gguf_architecture == "sharp");
    CHECK(sharp.converter_version == 1);
    CHECK(contains(sharp.source_url, "ml-site.cdn-apple.com/models/sharp/"));
    CHECK(contains(sharp.license_url, "ml-sharp"));
    CHECK(sharp.license_url.rfind("https://", 0) == 0);

    const ModelBackendSpec& da2 = splat::da2_model_spec();
    CHECK(da2.name == "da2");
    CHECK(da2.source_filename == "model.safetensors");  // the file behind the HF URL
    CHECK(da2.source_sha256 == splat::kDepthAnythingV2SmallSha256);
    CHECK(da2.source_bytes == splat::kDepthAnythingV2SmallBytes);
    CHECK(da2.gguf_architecture == "depth-anything-v2");
    CHECK(da2.license_url.empty());  // Apache-2.0, enabled by default
    CHECK(da2.license_name == "Apache-2.0");

    splat_test::TempDir temp("model-specs");
    ModelManagerOptions options;
    options.state_dir = temp.path();
    ModelManager manager(std::move(options));  // empty specs: both built-ins
    REQUIRE(manager.backend("sharp") != nullptr);
    REQUIRE(manager.backend("da2") != nullptr);
    CHECK(manager.backend("nope") == nullptr);
    CHECK(manager.backend_names() == std::vector<std::string>{"sharp", "da2"});
}

TEST_CASE("model manager: the licence gate blocks fetch and import") {
    splat_test::TempDir temp("model-licence-gate");
    const std::string payload = make_payload(2048);
    const ModelBackendSpec spec = small_spec("sharp", "http://127.0.0.1:1/never", payload);
    TestFetcher fetcher;
    ModelManager manager(options_for(temp.path(), spec, &fetcher));

    const Result<ModelRecord> initial = manager.status("sharp");
    REQUIRE_MESSAGE(initial.ok(), initial.message());
    CHECK(initial.value().state == ModelState::LicenseRequired);
    CHECK_FALSE(manager.license_accepted("sharp"));

    Progress progress;
    const Status fetched = manager.fetch("sharp", progress);
    REQUIRE_FALSE(fetched.ok());
    CHECK(contains(fetched.message(), "licence"));
    CHECK(fetcher.head_calls == 0);  // nothing touched the network

    const Path file = temp.path() / "model.bin";
    write_file(file, payload);
    const Status imported = manager.import("sharp", file, progress);
    REQUIRE_FALSE(imported.ok());
    CHECK(contains(imported.message(), "licence"));
    CHECK_FALSE(std::filesystem::exists(temp.path() / "downloads" / "model.bin"));

    const Result<ModelRecord> blocked = manager.status("sharp");
    REQUIRE(blocked.ok());
    CHECK(blocked.value().state == ModelState::LicenseRequired);
    CHECK(contains(blocked.value().message, "licence"));
}

TEST_CASE("model manager: acceptance stores the hash of the licence text shown") {
    splat_test::TempDir temp("model-accept");
    RangeServer server(make_payload(512));
    ModelBackendSpec spec = small_spec("sharp", server.url(), make_payload(512));
    spec.license_url = server.license_url();
    TestFetcher fetcher;
    ModelManager manager(options_for(temp.path(), spec, &fetcher));

    const Result<std::string> licence = manager.fetch_license_text("sharp");
    REQUIRE_MESSAGE(licence.ok(), licence.message());
    CHECK(licence.value() == server.license_text());

    REQUIRE(manager.accept_license("sharp", licence.value()).ok());
    CHECK(manager.license_accepted("sharp"));

    const Result<splat::LicenseState> state = splat::read_license_state(temp.path());
    REQUIRE_MESSAGE(state.ok(), state.message());
    REQUIRE(state.value().accepted.size() == 1);
    CHECK(state.value().accepted[0].backend == "sharp");
    CHECK(state.value().accepted[0].license_sha256 == sha256_of(licence.value()));
    CHECK_FALSE(state.value().accepted[0].accepted_at.empty());

    // The fallback needs no acceptance, and accepting it is an error.
    CHECK_FALSE(manager.fetch_license_text("da2").ok());
    CHECK_FALSE(manager.accept_license("da2", "apache").ok());
    CHECK_FALSE(manager.accept_license("sharp", "").ok());
}

TEST_CASE("model manager: an interrupted download resumes from the .part file") {
    splat_test::TempDir temp("model-resume");
    const std::string payload = make_payload(5000);
    RangeServer server(payload);
    const ModelBackendSpec spec = small_spec("sharp", server.url(), payload);
    TestFetcher fetcher;
    ModelManager manager(options_for(temp.path(), spec, &fetcher));
    REQUIRE(manager.accept_license("sharp", "accepted").ok());

    fetcher.abort_after = 1200;
    Progress progress;
    const Status interrupted = manager.fetch("sharp", progress);
    REQUIRE_FALSE(interrupted.ok());
    CHECK(contains(interrupted.message(), "interrupted"));

    const Path part = temp.path() / "downloads" / "model.bin.part";
    REQUIRE(std::filesystem::exists(part));
    CHECK(std::filesystem::file_size(part) == 1200);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "downloads" / "model.bin"));

    const Result<ModelRecord> paused = manager.status("sharp");
    REQUIRE(paused.ok());
    CHECK(paused.value().state == ModelState::Failed);
    CHECK(paused.value().downloaded_bytes == 1200);

    // Second attempt: the manager asks for the remaining bytes and finishes.
    fetcher.abort_after = 0;
    const Status resumed = manager.fetch("sharp", progress);
    REQUIRE_MESSAGE(resumed.ok(), resumed.message());

    const Result<ModelRecord> ready = manager.status("sharp");
    REQUIRE_MESSAGE(ready.ok(), ready.message());
    CHECK(ready.value().state == ModelState::Ready);
    CHECK(ready.value().source_bytes == payload.size());
    CHECK(ready.value().source_sha256 == sha256_of(payload));
    CHECK(ready.value().source_path == temp.path() / "downloads" / "model.bin");
    CHECK(ready.value().gguf_path == temp.path() / "models" / "sharp.gguf");
    CHECK(std::filesystem::exists(ready.value().gguf_path));

    CHECK_FALSE(std::filesystem::exists(part));
    CHECK(read_file(temp.path() / "downloads" / "model.bin") == payload);
    REQUIRE(manager.verify_gguf("sharp", ready.value().gguf_path).ok());

    const std::vector<uint64_t> ranges = server.range_starts();
    REQUIRE(ranges.size() == 1);  // the first attempt sent no Range header
    CHECK(ranges.front() == 1200);
    CHECK(progress.fraction() == doctest::Approx(1.0f));
}

TEST_CASE("model manager: a wrong hash is rejected and the .part is removed") {
    splat_test::TempDir temp("model-bad-hash");
    const std::string payload = make_payload(3000);
    RangeServer server(payload);
    ModelBackendSpec spec = small_spec("sharp", server.url(), payload);
    spec.source_sha256 = sha256_of("a different checkpoint, same length...");
    TestFetcher fetcher;
    ModelManager manager(options_for(temp.path(), spec, &fetcher));
    REQUIRE(manager.accept_license("sharp", "accepted").ok());

    Progress progress;
    const Status failed = manager.fetch("sharp", progress);
    REQUIRE_FALSE(failed.ok());
    CHECK(contains(failed.message(), "SHA-256"));

    CHECK_FALSE(std::filesystem::exists(temp.path() / "downloads" / "model.bin.part"));
    CHECK_FALSE(std::filesystem::exists(temp.path() / "downloads" / "model.bin"));
    CHECK_FALSE(std::filesystem::exists(temp.path() / "models" / "sharp.gguf"));

    const Result<ModelRecord> record = manager.status("sharp");
    REQUIRE(record.ok());
    CHECK(record.value().state == ModelState::Failed);
    CHECK(contains(record.value().message, "SHA-256"));
}

TEST_CASE("model manager: the inbox is picked up by file name and hash") {
    splat_test::TempDir temp("model-inbox");
    const std::string payload = make_payload(2500);
    const ModelBackendSpec spec = small_spec("da2", "http://127.0.0.1:1/never", payload);
    ModelManager manager(options_for(temp.path(), spec, nullptr));  // offline

    const Path inbox = manager.inbox_dir() / "model.bin";
    write_file(inbox, payload);

    Progress progress;
    const Status picked = manager.fetch("da2", progress);
    REQUIRE_MESSAGE(picked.ok(), picked.message());

    const Result<ModelRecord> ready = manager.status("da2");
    REQUIRE(ready.ok());
    CHECK(ready.value().state == ModelState::Ready);
    CHECK(read_file(temp.path() / "downloads" / "model.bin") == payload);
    CHECK_FALSE(std::filesystem::exists(inbox));  // moved out of the inbox
    CHECK(std::filesystem::exists(ready.value().gguf_path));

    // A different file name in the inbox is not picked up.
    splat_test::TempDir other("model-inbox-name");
    ModelManager other_manager(options_for(other.path(), spec, nullptr));
    write_file(other_manager.inbox_dir() / "something-else.bin", payload);
    const Status ignored = other_manager.fetch("da2", progress);
    REQUIRE_FALSE(ignored.ok());
    CHECK(contains(ignored.message(), "no HTTP fetcher"));
    CHECK(std::filesystem::exists(other_manager.inbox_dir() / "something-else.bin"));

    // The right name with the wrong bytes is rejected, and the user's file stays.
    splat_test::TempDir corrupt("model-inbox-hash");
    ModelManager corrupt_manager(options_for(corrupt.path(), spec, nullptr));
    const std::string wrong = make_payload(payload.size() - 1) + 'x';
    write_file(corrupt_manager.inbox_dir() / "model.bin", wrong);
    const Status rejected = corrupt_manager.fetch("da2", progress);
    REQUIRE_FALSE(rejected.ok());
    CHECK(contains(rejected.message(), "SHA-256"));
    CHECK(std::filesystem::exists(corrupt_manager.inbox_dir() / "model.bin"));
    CHECK_FALSE(std::filesystem::exists(corrupt.path() / "downloads" / "model.bin"));
}

TEST_CASE("model manager: a complete .part is verified after a crash") {
    splat_test::TempDir temp("model-complete-part");
    const std::string payload = make_payload(900);
    const ModelBackendSpec spec = small_spec("da2", "http://127.0.0.1:1/never", payload);
    ModelManager manager(options_for(temp.path(), spec, nullptr));  // no network at all

    // A crash after the last byte but before the rename leaves a full .part.
    const Path part = temp.path() / "downloads" / "model.bin.part";
    write_file(part, payload);

    Progress progress;
    const Status fetched = manager.fetch("da2", progress);
    REQUIRE_MESSAGE(fetched.ok(), fetched.message());
    CHECK_FALSE(std::filesystem::exists(part));
    CHECK(read_file(temp.path() / "downloads" / "model.bin") == payload);
    CHECK(manager.status("da2").value().state == ModelState::Ready);
}

TEST_CASE("model manager: the ready state survives a restart and revalidates the GGUF") {
    splat_test::TempDir temp("model-persist");
    const std::string payload = make_payload(4000);
    RangeServer server(payload);
    const ModelBackendSpec spec = small_spec("da2", server.url(), payload);
    TestFetcher fetcher;

    {
        ModelManager manager(options_for(temp.path(), spec, &fetcher));
        Progress progress;
        REQUIRE_MESSAGE(manager.fetch("da2", progress).ok(), "first fetch");
        const Result<ModelRecord> record = manager.status("da2");
        REQUIRE(record.ok());
        CHECK(record.value().state == ModelState::Ready);
    }

    // A fresh manager reads the JSON state file, not any in-memory state.
    ModelManager restarted(options_for(temp.path(), spec, &fetcher));
    const Result<ModelRecord> after_restart = restarted.status("da2");
    REQUIRE_MESSAGE(after_restart.ok(), after_restart.message());
    CHECK(after_restart.value().state == ModelState::Ready);
    CHECK(after_restart.value().gguf_path == temp.path() / "models" / "da2.gguf");
    const Path good_gguf = after_restart.value().gguf_path;

    // Metadata from another source checkpoint means the file is stale.
    {
        Progress progress;
        const Result<ConvertStats> bad = write_fake_gguf(spec, good_gguf, progress, std::string(64, '0'));
        REQUIRE_MESSAGE(bad.ok(), bad.message());
    }
    const Result<ModelRecord> tampered = restarted.status("da2");
    REQUIRE(tampered.ok());
    CHECK(tampered.value().state == ModelState::Failed);
    CHECK(contains(tampered.value().message, "source SHA-256"));

    // Fetching again reconverts from the verified source and becomes ready.
    Progress progress;
    REQUIRE_MESSAGE(restarted.fetch("da2", progress).ok(), "recovery fetch");
    CHECK(restarted.status("da2").value().state == ModelState::Ready);
    CHECK(restarted.verify_gguf("da2", good_gguf).ok());

    // A missing runtime file is a failure with a message, not an exception.
    std::filesystem::remove(good_gguf);
    const Result<ModelRecord> missing = restarted.status("da2");
    REQUIRE(missing.ok());
    CHECK(missing.value().state == ModelState::Failed);
    CHECK(contains(missing.value().message, "no such file"));
}

TEST_CASE("model manager: a stale converter version is rejected on load") {
    splat_test::TempDir temp("model-converter-version");
    const std::string payload = make_payload(1000);
    RangeServer server(payload);
    const ModelBackendSpec spec = small_spec("da2", server.url(), payload);
    TestFetcher fetcher;
    ModelManager manager(options_for(temp.path(), spec, &fetcher));

    Progress progress;
    REQUIRE_MESSAGE(manager.fetch("da2", progress).ok(), "fetch");
    const Path gguf = temp.path() / "models" / "da2.gguf";
    REQUIRE_MESSAGE(
        write_fake_gguf(spec, gguf, progress, std::string(), 2).ok(), "write the stale GGUF");

    const Result<ModelRecord> record = manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == ModelState::Failed);
    CHECK(contains(record.value().message, "converter version"));
}

TEST_CASE("model manager: a transient state left by a crash is reconciled") {
    splat_test::TempDir temp("model-crash");
    const std::string payload = make_payload(1500);
    const ModelBackendSpec spec = small_spec("da2", "http://127.0.0.1:1/never", payload);
    ModelManager manager(options_for(temp.path(), spec, nullptr));

    // Simulate a crash during the download: state on disk says downloading and
    // a .part of 700 bytes is there.
    const Path part = temp.path() / "downloads" / "model.bin.part";
    write_file(part, payload.substr(0, 700));
    nlohmann::json state;
    state["format_version"] = 1;
    state["backend"] = "da2";
    state["state"] = "downloading";
    state["message"] = "";
    state["source_sha256"] = "";
    state["source_bytes"] = payload.size();
    state["downloaded_bytes"] = 700;
    state["source_path"] = "";
    state["gguf_path"] = "";
    state["converter_version"] = 1;
    state["updated_at"] = "2026-10-26T00:00:00Z";
    write_file(temp.path() / "models" / "da2.json", state.dump(2));

    const Result<ModelRecord> resumed = manager.status("da2");
    REQUIRE(resumed.ok());
    CHECK(resumed.value().state == ModelState::Accepted);
    CHECK(resumed.value().downloaded_bytes == 700);
    CHECK(contains(resumed.value().message, "paused"));

    // Simulate a crash during conversion: that restarts from the source file.
    nlohmann::json converting = state;
    converting["state"] = "converting";
    write_file(temp.path() / "models" / "da2.json", converting.dump(2));
    const Result<ModelRecord> failed = manager.status("da2");
    REQUIRE(failed.ok());
    CHECK(failed.value().state == ModelState::Failed);
    CHECK(contains(failed.value().message, "interrupted"));
}

TEST_CASE("model manager: import requires the official file name") {
    splat_test::TempDir temp("model-import");
    const std::string payload = make_payload(1234);
    const ModelBackendSpec spec = small_spec("da2", "http://127.0.0.1:1/never", payload);
    ModelManager manager(options_for(temp.path(), spec, nullptr));

    const Path wrong_name = temp.path() / "weights.bin";
    write_file(wrong_name, payload);
    Progress progress;
    const Status unknown = manager.import_file(wrong_name, progress);
    REQUIRE_FALSE(unknown.ok());
    CHECK(contains(unknown.message(), "unknown model file name"));

    const Status rejected = manager.import("da2", wrong_name, progress);
    REQUIRE_FALSE(rejected.ok());
    CHECK(contains(rejected.message(), "official file name"));

    const Path named = temp.path() / "model.bin";
    write_file(named, payload);
    const Status imported = manager.import_file(named, progress);
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    const Result<ModelRecord> record = manager.status("da2");
    REQUIRE(record.ok());
    CHECK(record.value().state == ModelState::Ready);
    CHECK(std::filesystem::exists(named));  // a user copy is not moved
    CHECK(std::filesystem::exists(temp.path() / "downloads" / "model.bin"));
}
