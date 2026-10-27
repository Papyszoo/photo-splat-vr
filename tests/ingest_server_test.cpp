#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "httplib/httplib.h"
#include "ingest/ingest_server.h"
#include "jobs/job_queue.h"
#include "test_util.h"

namespace {

std::string jpeg_bytes(size_t size = 256) {
    std::string data;
    data.push_back(static_cast<char>(0xff));
    data.push_back(static_cast<char>(0xd8));
    data.push_back(static_cast<char>(0xff));
    data.append(size, 'j');
    return data;
}

splat::IngestServerConfig local_config(const splat::Path& inbox) {
    splat::IngestServerConfig config;
    config.inbox = inbox;
    config.bind_address = "127.0.0.1";
    config.port = 0;  // let the OS pick a free port
    return config;
}

std::string login(httplib::Client& client, const std::string& pin) {
    const auto response =
        client.Post("/api/login", "pin=" + pin, "application/x-www-form-urlencoded");
    REQUIRE(response != nullptr);
    REQUIRE(response->status == 200);
    const std::string header = response->get_header_value("Set-Cookie");
    const std::string prefix = "splat_session=";
    REQUIRE(header.compare(0, prefix.size(), prefix) == 0);
    const size_t end = header.find(';');
    const std::string token =
        header.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
    REQUIRE(token.size() == 32);
    CHECK(header.find("HttpOnly") != std::string::npos);
    client.set_default_headers({{"Cookie", prefix + token}});
    return token;
}

splat::QueueSnapshot snapshot(const splat::JobQueue& queue) {
    splat::QueueSnapshot entries;
    for (const splat::Job& job : queue.jobs()) {
        splat::QueueEntry entry;
        entry.id = job.id;
        entry.photo = job.photo.string();
        entry.state = splat::job_state_name(job.state);
        entry.message = job.message;
        entry.seconds_taken = job.seconds_taken;
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace

TEST_CASE("the embedded page is self-contained and needs no internet") {
    splat_test::TempDir dir("ingest-page");
    splat::IngestServer server(local_config(dir.path() / "inbox"));
    const splat::Status started = server.start();
    REQUIRE_MESSAGE(started.ok(), started.message());
    CHECK(server.running());
    CHECK(server.bind_address() == "127.0.0.1");
    CHECK(server.port() != 0);
    CHECK(server.pin().size() == 6);

    httplib::Client client("127.0.0.1", server.port());
    const auto page = client.Get("/");
    REQUIRE(page != nullptr);
    CHECK(page->status == 200);
    CHECK(page->get_header_value("Content-Type").find("text/html") != std::string::npos);
    CHECK(page->body.find("Enter the PIN") != std::string::npos);
    CHECK(page->body.find("type=\"file\"") != std::string::npos);
    CHECK(page->body.find("APPLE") == std::string::npos);
    // No CDN, font or script hosted elsewhere.
    CHECK(page->body.find("http://") == std::string::npos);
    CHECK(page->body.find("https://") == std::string::npos);
    CHECK(page->body.find("<script src") == std::string::npos);
    CHECK(page->body.find("<link ") == std::string::npos);
}

TEST_CASE("the queue endpoint requires the PIN and then accepts the session cookie") {
    splat_test::TempDir dir("ingest-auth");
    splat::IngestServer server(local_config(dir.path() / "inbox"));
    REQUIRE(server.start().ok());

    httplib::Client anonymous("127.0.0.1", server.port());
    const auto denied = anonymous.Get("/api/queue");
    REQUIRE(denied != nullptr);
    CHECK(denied->status == 401);
    CHECK(denied->body.find("enter the PIN") != std::string::npos);

    httplib::Client phone("127.0.0.1", server.port());
    const auto wrong = phone.Post("/api/login", "pin=000000", "application/x-www-form-urlencoded");
    REQUIRE(wrong != nullptr);
    CHECK(wrong->status == 401);
    CHECK(wrong->get_header_value("Set-Cookie").empty());
    CHECK(phone.Get("/api/queue")->status == 401);

    login(phone, server.pin());
    const auto allowed = phone.Get("/api/queue");
    REQUIRE(allowed != nullptr);
    CHECK(allowed->status == 200);
    const nlohmann::json body = nlohmann::json::parse(allowed->body);
    CHECK(body.at("ok") == true);
    CHECK(body.at("jobs").is_array());
    CHECK(body.at("counts").at("queued") == 0);
}

TEST_CASE("five wrong PINs over HTTP lock the phone out with Retry-After") {
    splat_test::TempDir dir("ingest-lockout");
    splat::IngestServerConfig config = local_config(dir.path() / "inbox");
    config.pin.max_attempts = 5;
    config.pin.lockout = std::chrono::seconds(60);
    splat::IngestServer server(std::move(config));
    REQUIRE(server.start().ok());

    httplib::Client phone("127.0.0.1", server.port());
    for (int attempt = 1; attempt <= 4; ++attempt) {
        const auto response = phone.Post("/api/login", "pin=111111",
                                         "application/x-www-form-urlencoded");
        REQUIRE(response != nullptr);
        CHECK(response->status == 401);
    }
    const auto fifth =
        phone.Post("/api/login", "pin=111111", "application/x-www-form-urlencoded");
    REQUIRE(fifth != nullptr);
    CHECK(fifth->status == 429);
    CHECK(fifth->get_header_value("Retry-After") == "60");

    // Correct PIN during the lockout is still refused.
    const auto correct = phone.Post("/api/login", "pin=" + server.pin(),
                                    "application/x-www-form-urlencoded");
    REQUIRE(correct != nullptr);
    CHECK(correct->status == 429);
    CHECK(correct->get_header_value("Set-Cookie").empty());
}

TEST_CASE("upload needs a session and rejects bad content, names and sizes") {
    splat_test::TempDir dir("ingest-http-upload");
    const splat::Path inbox = dir.path() / "inbox";
    splat::IngestServer server(local_config(inbox));
    REQUIRE(server.start().ok());

    httplib::Client anonymous("127.0.0.1", server.port());
    const httplib::MultipartFormDataItems photo{
        {"photo", jpeg_bytes(), "IMG_0001.jpg", "image/jpeg"}};
    const auto denied = anonymous.Post("/api/upload", photo);
    REQUIRE(denied != nullptr);
    CHECK(denied->status == 401);

    httplib::Client phone("127.0.0.1", server.port());
    login(phone, server.pin());

    const auto bad_magic =
        phone.Post("/api/upload",
                   httplib::MultipartFormDataItems{{"photo", "not an image", "evil.jpg",
                                                   "image/jpeg"}});
    REQUIRE(bad_magic != nullptr);
    CHECK(bad_magic->status == 400);
    CHECK(nlohmann::json::parse(bad_magic->body).at("error").get<std::string>().find("JPEG") !=
          std::string::npos);

    const auto traversal = phone.Post(
        "/api/upload",
        httplib::MultipartFormDataItems{{"photo", jpeg_bytes(), "../escape.jpg", "image/jpeg"}});
    REQUIRE(traversal != nullptr);
    CHECK(traversal->status == 400);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "escape.jpg"));

    const auto missing = phone.Post(
        "/api/upload", httplib::MultipartFormDataItems{{"other", jpeg_bytes(), "x.jpg", "image/jpeg"}});
    REQUIRE(missing != nullptr);
    CHECK(missing->status == 400);

    const auto accepted = phone.Post("/api/upload", photo);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->status == 200);
    const nlohmann::json body = nlohmann::json::parse(accepted->body);
    CHECK(body.at("ok") == true);
    CHECK(body.at("bytes") == jpeg_bytes().size());
    const std::string stored = body.at("file").get<std::string>();
    CHECK(stored.substr(stored.size() - 4) == ".jpg");
    CHECK(std::filesystem::exists(inbox / stored));
}

TEST_CASE("an oversized upload is refused with 413") {
    splat_test::TempDir dir("ingest-http-size");
    splat::IngestServerConfig config = local_config(dir.path() / "inbox");
    config.max_upload_bytes = 64;
    splat::IngestServer server(std::move(config));
    REQUIRE(server.start().ok());

    httplib::Client phone("127.0.0.1", server.port());
    login(phone, server.pin());
    const auto response = phone.Post(
        "/api/upload",
        httplib::MultipartFormDataItems{{"photo", jpeg_bytes(128), "big.jpg", "image/jpeg"}});
    REQUIRE(response != nullptr);
    CHECK(response->status == 413);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "inbox"));
}

TEST_CASE("the server refuses to bind every interface or a public address") {
    splat_test::TempDir dir("ingest-bind");
    for (const std::string address : {"0.0.0.0", "8.8.8.8", "example.com"}) {
        CAPTURE(address);
        splat::IngestServerConfig config = local_config(dir.path() / "inbox");
        config.bind_address = address;
        splat::IngestServer server(std::move(config));
        const splat::Status started = server.start();
        CHECK_FALSE(started.ok());
        CHECK(started.message().find("LAN-only") != std::string::npos);
        CHECK_FALSE(server.running());
    }
}

TEST_CASE("the queue JSON reflects queued, running, done and failed jobs") {
    splat_test::TempDir dir("ingest-queue-json");
    std::mutex queue_mutex;
    splat::JobQueue queue =
        splat::JobQueue::open(dir.path() / "state" / "jobs.json").value();

    const auto done = queue.enqueue(dir.path() / "done.jpg", "sharp");
    const auto failed = queue.enqueue(dir.path() / "failed.jpg", "fallback");
    const auto running = queue.enqueue(dir.path() / "running.jpg", "sharp");
    const auto queued = queue.enqueue(dir.path() / "queued.jpg", "fallback");
    REQUIRE(done.ok());
    REQUIRE(failed.ok());
    REQUIRE(running.ok());
    REQUIRE(queued.ok());

    const auto first = queue.take_next();
    REQUIRE(first.ok());
    REQUIRE(first.value().has_value());
    REQUIRE(queue.mark_done(first.value()->id, dir.path() / "scene", 1.5).ok());

    const auto second = queue.take_next();
    REQUIRE(second.ok());
    REQUIRE(second.value().has_value());
    REQUIRE(queue.mark_failed(second.value()->id, "decode failed").ok());

    const auto third = queue.take_next();
    REQUIRE(third.ok());
    REQUIRE(third.value().has_value());

    splat::IngestServerConfig config = local_config(dir.path() / "inbox");
    config.queue = [&queue, &queue_mutex]() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return snapshot(queue);
    };
    splat::IngestServer server(std::move(config));
    REQUIRE(server.start().ok());

    httplib::Client phone("127.0.0.1", server.port());
    login(phone, server.pin());
    const auto response = phone.Get("/api/queue");
    REQUIRE(response != nullptr);
    REQUIRE(response->status == 200);
    const nlohmann::json body = nlohmann::json::parse(response->body);

    REQUIRE(body.at("jobs").size() == 4);
    std::map<std::string, nlohmann::json> by_id;
    for (const auto& job : body.at("jobs")) {
        by_id[job.at("id").get<std::string>()] = job;
        // Only the base name is exposed, never the host inbox path.
        CHECK(job.at("photo").get<std::string>().find('/') == std::string::npos);
    }

    CHECK(by_id.at(done.value()).at("state") == "done");
    CHECK(by_id.at(done.value()).at("photo") == "done.jpg");
    CHECK(by_id.at(done.value()).at("seconds_taken") == doctest::Approx(1.5));
    CHECK(by_id.at(failed.value()).at("state") == "failed");
    CHECK(by_id.at(failed.value()).at("message") == "decode failed");
    CHECK(by_id.at(running.value()).at("state") == "running");
    CHECK(by_id.at(queued.value()).at("state") == "queued");

    const nlohmann::json counts = body.at("counts");
    CHECK(counts.at("queued") == 1);
    CHECK(counts.at("running") == 1);
    CHECK(counts.at("done") == 1);
    CHECK(counts.at("failed") == 1);
    CHECK(counts.at("cancelled") == 0);
}
