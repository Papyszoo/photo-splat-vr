// AsyncListing: a local directory is ready before request() returns; a location
// path is listed by the worker, so request() returns at once, busy() is true,
// tick() publishes the entries, and the UI thread never blocks.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/path.h"
#include "test_util.h"
#include "vfs/io.h"
#include "vfs/listing.h"

using splat::Path;
namespace vfs = splat::vfs;

namespace {

// The three names every fake listing returns, after sleeping 200 ms so a
// blocking request would be obvious.
class SlowFakeFileSystem : public vfs::IFileSystem {
public:
    std::string id() const override { return "slow"; }
    std::string describe() const override { return "slow:/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string&, size_t) override {
        return splat::Result<std::vector<uint8_t>>::failure("not used");
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::success();
    }
    splat::Status list(const std::string&, std::vector<vfs::DirEntry>* entries) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (const char* name : {"one.psplat", "two.psplat", "three.jpg"}) {
            vfs::DirEntry entry;
            entry.name = name;
            entry.size = 4;
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string&, vfs::FileStat* out) override {
        *out = vfs::FileStat{};
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }
};

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// A listing whose answer (and delay) depends on the directory, so a result
// published under the wrong request shows up as the wrong names, and a dropped
// answer leaves the next one visibly in flight.
class NamedFakeFileSystem : public vfs::IFileSystem {
public:
    std::string id() const override { return "named"; }
    std::string describe() const override { return "named:/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string&, size_t) override {
        return splat::Result<std::vector<uint8_t>>::failure("not used");
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::success();
    }
    splat::Status list(const std::string& relative, std::vector<vfs::DirEntry>* entries) override {
        // "a" answers quickly and "b" slowly: after the supersede, A's answer is
        // already dropped while B is still with the worker.
        const bool second = relative == "b";
        std::this_thread::sleep_for(std::chrono::milliseconds(second ? 700 : 100));
        const std::vector<std::string> names =
            second ? std::vector<std::string>{"b-one", "b-two"} : std::vector<std::string>{"a-one"};
        for (const std::string& name : names) {
            vfs::DirEntry entry;
            entry.name = name;
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string&, vfs::FileStat* out) override {
        *out = vfs::FileStat{};
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }
};

// Polls tick() the way a screen does.
bool wait_ready(vfs::AsyncListing& listing, int milliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!listing.ready() && std::chrono::steady_clock::now() < deadline) {
        listing.tick();
        if (!listing.ready()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return listing.ready();
}

}  // namespace

TEST_CASE("AsyncListing lists a local directory inside request") {
    splat_test::TempDir dir("vfs-listing-local");
    {
        std::ofstream(dir.path() / "a.txt") << "a";
        std::ofstream(dir.path() / "b.txt") << "b";
    }
    vfs::AsyncListing listing;
    listing.request(dir.path());
    CHECK_FALSE(listing.busy());
    CHECK(listing.ready());
    CHECK(listing.entries().size() == 2);
}

TEST_CASE("AsyncListing returns immediately and publishes a slow location in tick") {
    auto slow = std::make_shared<SlowFakeFileSystem>();
    vfs::file_systems().add(slow);
    struct Remove {
        ~Remove() { vfs::file_systems().remove("slow"); }
    } removed;

    vfs::AsyncListing listing;
    const auto started = std::chrono::steady_clock::now();
    listing.request(Path("slow:/photos"));
    const double request_ms = milliseconds_since(started);

    CHECK(request_ms < 20.0);
    CHECK(listing.busy());
    CHECK_FALSE(listing.ready());

    // Poll tick() the way a screen does, up to two seconds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!listing.ready() && std::chrono::steady_clock::now() < deadline) {
        listing.tick();
        if (!listing.ready()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(listing.ready());
    CHECK_FALSE(listing.busy());
    REQUIRE(listing.entries().size() == 3);
    CHECK(listing.entries()[0].name == "one.psplat");
    CHECK(listing.entries()[1].name == "two.psplat");
    CHECK(listing.entries()[2].name == "three.jpg");
    CHECK(listing.error().empty());
}

TEST_CASE("a superseded listing is dropped, not published under the new request") {
    auto named = std::make_shared<NamedFakeFileSystem>();
    vfs::file_systems().add(named);
    struct Remove {
        ~Remove() { vfs::file_systems().remove("named"); }
    } removed;

    vfs::AsyncListing listing;
    listing.request(Path("named:/a"));
    CHECK(listing.busy());
    // The worker has almost certainly taken A; a second request before it
    // answers is what a fast d-pad does on a share.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    listing.request(Path("named:/b"));

    REQUIRE(wait_ready(listing, 2000));
    // B's entries, never A's: A's answer arrived after the request moved on.
    REQUIRE(listing.entries().size() == 2);
    CHECK(listing.entries()[0].name == "b-one");
    CHECK(listing.entries()[1].name == "b-two");
    CHECK(listing.directory() == Path("named:/b"));
    CHECK_FALSE(listing.busy());
}

TEST_CASE("a dropped result keeps busy until the live listing answers") {
    auto named = std::make_shared<NamedFakeFileSystem>();
    vfs::file_systems().add(named);
    struct Remove {
        ~Remove() { vfs::file_systems().remove("named"); }
    } removed;

    vfs::AsyncListing listing;
    listing.request(Path("named:/a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    listing.request(Path("named:/b"));

    // A answers at ~100 ms and is dropped; B needs ~700 ms. Halfway, the screen
    // must still say "Loading..." rather than paint the empty grid.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    listing.tick();
    CHECK_FALSE(listing.ready());
    CHECK(listing.busy());
    CHECK(listing.entries().empty());

    REQUIRE(wait_ready(listing, 2000));
    CHECK_FALSE(listing.busy());
    REQUIRE(listing.entries().size() == 2);
    CHECK(listing.entries()[0].name == "b-one");
}

TEST_CASE("cancel drops an answer that is already in flight") {
    auto named = std::make_shared<NamedFakeFileSystem>();
    vfs::file_systems().add(named);
    struct Remove {
        ~Remove() { vfs::file_systems().remove("named"); }
    } removed;

    vfs::AsyncListing listing;
    listing.request(Path("named:/a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    listing.cancel();  // the screen left while the worker was listing
    CHECK_FALSE(listing.busy());
    CHECK_FALSE(listing.ready());

    // Give the worker time to finish A and let tick() see it.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    listing.tick();
    CHECK_FALSE(listing.ready());
    CHECK(listing.entries().empty());

    // The listing is usable again afterwards.
    listing.request(Path("named:/b"));
    REQUIRE(wait_ready(listing, 2000));
    REQUIRE(listing.entries().size() == 2);
    CHECK(listing.entries()[0].name == "b-one");
}
