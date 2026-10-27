// W5. Host tests for HelperClient against the real protocol functions: a small
// fake helper loop reads req.json, answers through splat::handle_request (the
// same code the payload runs) and rewrites the heartbeat.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "capture/helper_client.h"
#include "capture/helper_protocol.h"
#include "test_util.h"

namespace {

namespace fs = std::filesystem;
using splat::HelperClient;
using splat::Path;

void write_file(const Path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string read_file(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

long long unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The console layout: photo/<service>/<title>/<hash>/<capture>.
struct Fixture {
    splat_test::TempDir dir;
    Path helper_dir;
    Path root;
    Path inbox;

    explicit Fixture(const char* tag)
        : dir(tag), helper_dir(dir.path() / "helper"), root(dir.path() / "av"), inbox(dir.path() / "inbox") {
        fs::create_directories(helper_dir);
        write_file(root / "photo" / "NPXS40087" / "CUSA12392" / "2c4" / "20261024_101112_aabbccdd.jpg",
                   std::string(128, 'j'));
        write_file(root / "photo" / "NPXS40087" / "CUSA12392" / "2c4" / "20261025_052805_00338445.jxr",
                   std::string(64, 'x'));
        // The helper reads its root from root.txt, exactly like the payload.
        write_file(helper_dir / splat::kHelperRootName, root.string() + "\n");
    }

    Path jpg() const {
        return root / "photo" / "NPXS40087" / "CUSA12392" / "2c4" / "20261024_101112_aabbccdd.jpg";
    }
    Path jxr() const {
        return root / "photo" / "NPXS40087" / "CUSA12392" / "2c4" / "20261025_052805_00338445.jxr";
    }

    // Heartbeat, rewritten every 2 s by the payload.
    void beat(long long offset_seconds = 0) {
        write_file(helper_dir / splat::kHelperAliveName, std::to_string(unix_now() + offset_seconds));
    }

    // One fake helper pass: answer the pending request with the protocol code.
    bool answer() {
        const Path request = helper_dir / splat::kHelperRequestName;
        if (!fs::exists(request)) return false;
        const std::string response =
            splat::handle_request(read_file(request), splat::helper_root(helper_dir), inbox);
        write_file(helper_dir / "resp.tmp", response);
        fs::rename(helper_dir / "resp.tmp", helper_dir / splat::kHelperResponseName);
        fs::remove(request);
        return true;
    }
};

}  // namespace

TEST_CASE("helper client reports no helper when the heartbeat is missing") {
    Fixture fixture("helper-client-nohb");
    HelperClient client(fixture.helper_dir);

    CHECK_FALSE(client.alive());
    CHECK(client.state() == HelperClient::State::Idle);

    const splat::Status status = client.request_list();
    CHECK_FALSE(status.ok());
    CHECK(client.state() == HelperClient::State::NoHelper);
    CHECK(client.error().find("not running") != std::string::npos);

    // A stale heartbeat is "not running" too.
    fixture.beat(-30);
    CHECK_FALSE(client.alive());
    CHECK_FALSE(client.request_copy({fixture.jpg()}).ok());
    CHECK(client.state() == HelperClient::State::NoHelper);
}

TEST_CASE("helper client lists captures from the helper answer") {
    Fixture fixture("helper-client-list");
    HelperClient client(fixture.helper_dir);
    fixture.beat();
    CHECK(client.alive());

    REQUIRE(client.request_list().ok());
    CHECK(client.state() == HelperClient::State::Waiting);
    CHECK(client.captures().empty());

    // No answer yet: the client stays waiting, and poll() is non-blocking.
    client.poll();
    CHECK(client.state() == HelperClient::State::Waiting);

    REQUIRE(fixture.answer());
    client.poll();
    CHECK(client.state() == HelperClient::State::Done);
    CHECK(client.error().empty());
    REQUIRE(client.captures().size() == 2);
    CHECK(client.captures()[0].name == "20261025_052805_00338445.jxr");
    CHECK_FALSE(client.captures()[0].supported);
    CHECK(client.captures()[1].name == "20261024_101112_aabbccdd.jpg");
    CHECK(client.captures()[1].supported);
    CHECK(client.captures()[1].bytes == 128);
    CHECK(client.copied().empty());
}

TEST_CASE("helper client copies and reports the inbox destinations") {
    Fixture fixture("helper-client-copy");
    HelperClient client(fixture.helper_dir);
    fixture.beat();

    REQUIRE(client.request_copy({fixture.jpg()}).ok());
    CHECK(client.state() == HelperClient::State::Waiting);

    REQUIRE(fixture.answer());
    client.poll();
    CHECK(client.state() == HelperClient::State::Done);
    REQUIRE(client.copied().size() == 1);
    CHECK(client.copied()[0] == fixture.inbox / "capture-20261024_101112-aabbccdd.jpg");
    CHECK(fs::exists(client.copied()[0]));
    CHECK(fs::file_size(client.copied()[0]) == 128);
    CHECK(client.captures().empty());

    // A second request is a new mailbox round trip, answered with no copy.
    REQUIRE(client.request_copy({fixture.jpg()}).ok());
    REQUIRE(fixture.answer());
    client.poll();
    CHECK(client.state() == HelperClient::State::Done);
    CHECK(client.copied().empty());
}

TEST_CASE("helper client fails with a timeout when the helper stays silent") {
    Fixture fixture("helper-client-timeout");
    HelperClient::Timing timing;
    timing.timeout_seconds = 0.05;
    HelperClient client(fixture.helper_dir, timing);
    fixture.beat();

    REQUIRE(client.request_list().ok());
    client.poll();
    CHECK(client.state() == HelperClient::State::Waiting);

    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    client.poll();
    CHECK(client.state() == HelperClient::State::Failed);
    CHECK(client.error().find("timeout") != std::string::npos);
    CHECK(client.alive());  // the heartbeat is still fresh
}

TEST_CASE("helper client ignores an answer for another request id") {
    Fixture fixture("helper-client-stale");
    HelperClient client(fixture.helper_dir);
    fixture.beat();

    write_file(fixture.helper_dir / splat::kHelperResponseName,
               R"({"id":999,"ok":true,"captures":[],"copied":[],"error":""})");
    REQUIRE(client.request_list().ok());
    client.poll();
    CHECK(client.state() == HelperClient::State::Waiting);

    REQUIRE(fixture.answer());
    client.poll();
    CHECK(client.state() == HelperClient::State::Done);
    CHECK(client.captures().size() == 2);
}

TEST_CASE("helper client reports Failed when the helper refuses a copy") {
    Fixture fixture("helper-client-refused");
    HelperClient client(fixture.helper_dir);
    fixture.beat();

    REQUIRE(client.request_copy({fixture.jxr()}).ok());
    REQUIRE(fixture.answer());
    client.poll();
    CHECK(client.state() == HelperClient::State::Failed);
    CHECK(client.error().find("not supported") != std::string::npos);
    CHECK(client.copied().empty());
    CHECK_FALSE(fs::exists(fixture.inbox));
}

TEST_CASE("helper client goes to NoHelper when the heartbeat stops mid-request") {
    Fixture fixture("helper-client-dead");
    HelperClient client(fixture.helper_dir);
    fixture.beat();

    REQUIRE(client.request_list().ok());
    fixture.beat(-30);  // the payload died before answering
    client.poll();
    CHECK(client.state() == HelperClient::State::NoHelper);
    CHECK(client.error().find("not running") != std::string::npos);
}

TEST_CASE("helper start returns false on the host") {
    splat_test::TempDir dir("helper-client-start");
    CHECK_FALSE(splat::start_helper(dir.path() / "splat-helper.elf"));
}
