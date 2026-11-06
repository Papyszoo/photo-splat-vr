// core/vfs/location: the JSON round-trip, stable ids, the missing-file and
// corrupt-file cases, removal, and the 0600 mode of the credential file.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "common/path.h"
#include "test_util.h"
#include "vfs/io.h"
#include "vfs/location.h"

using splat::Path;
namespace vfs = splat::vfs;

namespace {

vfs::Location smb_location(const std::string& host, const std::string& share) {
    vfs::Location location;
    location.protocol = vfs::LocationProtocol::smb;
    location.host = host;
    location.share = share;
    location.user = "test";
    location.password = "secret";
    location.port = 1445;
    return location;
}

}  // namespace

TEST_CASE("locations round-trip through JSON") {
    const std::vector<vfs::Location> locations = [] {
        std::vector<vfs::Location> out;
        vfs::Location first = smb_location("nas.local", "Photos");
        first.id = "smb0";
        first.path = "scenes";
        first.encrypt = true;
        first.photo_source = true;
        first.splat_library = true;
        out.push_back(first);

        vfs::Location second;
        second.id = "nfs0";
        second.protocol = vfs::LocationProtocol::nfs;
        second.host = "192.168.1.20";
        second.share = "/export";
        second.nfs_version = 4;
        second.uid = 1000;
        second.gid = 1001;
        out.push_back(second);
        return out;
    }();

    const std::string json = vfs::locations_to_json(locations);
    const splat::Result<std::vector<vfs::Location>> parsed = vfs::locations_from_json(json);
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value().size() == 2);

    const vfs::Location& first = parsed.value()[0];
    CHECK(first.id == "smb0");
    CHECK(first.protocol == vfs::LocationProtocol::smb);
    CHECK(first.host == "nas.local");
    CHECK(first.share == "Photos");
    CHECK(first.path == "scenes");
    CHECK(first.user == "test");
    CHECK(first.password == "secret");
    CHECK(first.port == 1445);
    CHECK(first.encrypt);
    CHECK(first.photo_source);
    CHECK(first.splat_library);

    const vfs::Location& second = parsed.value()[1];
    CHECK(second.id == "nfs0");
    CHECK(second.protocol == vfs::LocationProtocol::nfs);
    CHECK(second.share == "/export");
    CHECK(second.nfs_version == 4);
    CHECK(second.uid == 1000);
    CHECK(second.gid == 1001);
    CHECK_FALSE(second.encrypt);
}

TEST_CASE("locations keep ids stable and unique") {
    splat_test::TempDir dir("vfs-locations");
    const Path file = dir.path() / "locations.json";

    splat::Result<vfs::LocationStore> opened = vfs::LocationStore::open(file);
    REQUIRE(opened.ok());
    vfs::LocationStore store = opened.take();
    CHECK(store.locations().empty());

    const splat::Result<std::string> first = store.add(smb_location("a.local", "One"));
    REQUIRE(first.ok());
    CHECK(first.value() == "smb0");
    const splat::Result<std::string> second = store.add(smb_location("b.local", "Two"));
    REQUIRE(second.ok());
    CHECK(second.value() == "smb1");

    vfs::Location nfs;
    nfs.protocol = vfs::LocationProtocol::nfs;
    nfs.host = "c.local";
    nfs.share = "/export";
    const splat::Result<std::string> third = store.add(nfs);
    REQUIRE(third.ok());
    CHECK(third.value() == "nfs0");

    // Reopening keeps the ids, and a later add never reuses a dropped one.
    splat::Result<vfs::LocationStore> reopened = vfs::LocationStore::open(file);
    REQUIRE(reopened.ok());
    CHECK(reopened.value().locations().size() == 3);
    REQUIRE(reopened.value().find("smb1") != nullptr);
    CHECK(reopened.value().find("smb1")->host == "b.local");

    REQUIRE(reopened.value().remove("smb0").ok());
    const splat::Result<std::string> fourth = reopened.value().add(smb_location("d.local", "Four"));
    REQUIRE(fourth.ok());
    CHECK(fourth.value() == "smb2");

    const splat::Result<vfs::LocationStore> again = vfs::LocationStore::open(file);
    REQUIRE(again.ok());
    CHECK(again.value().locations().size() == 3);
    CHECK(again.value().find("smb0") == nullptr);
    CHECK(again.value().find("smb2") != nullptr);

#if !defined(_WIN32)
    struct stat info {};
    REQUIRE(::stat(file.string().c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
#endif
}

TEST_CASE("a missing locations file is an empty store") {
    splat_test::TempDir dir("vfs-locations-missing");
    splat::Result<vfs::LocationStore> opened =
        vfs::LocationStore::open(dir.path() / "nothing.json");
    REQUIRE(opened.ok());
    CHECK(opened.value().locations().empty());
}

TEST_CASE("a corrupt locations file is an error, not a crash") {
    splat_test::TempDir dir("vfs-locations-corrupt");
    const Path file = dir.path() / "locations.json";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{ this is not json";
    }
    const splat::Result<vfs::LocationStore> opened = vfs::LocationStore::open(file);
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.message().find("locations.json") != std::string::npos);
}

TEST_CASE("an implausible locations file is refused before it is parsed") {
    splat_test::TempDir dir("vfs-locations-limits");
    const Path file = dir.path() / "locations.json";

    // One entry per element, so the count is what makes it impossible: the
    // reader must refuse it before it builds a Location or reserves a vector.
    std::string many = "{\"locations\":[";
    for (std::size_t i = 0; i <= vfs::LocationStore::kMaxEntries; ++i) {
        if (i != 0) many += ",";
        many += "{\"id\":\"smb" + std::to_string(i) + "\",\"host\":\"h\"}";
    }
    many += "]}";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << many;
    }
    const splat::Result<vfs::LocationStore> counted = vfs::LocationStore::open(file);
    REQUIRE_FALSE(counted.ok());
    CHECK(counted.message().find("entry limit") != std::string::npos);

    // A file past the byte limit is refused from its stat, before the read.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const std::string block(1 << 16, 'x');
        for (std::size_t i = 0; i <= vfs::LocationStore::kMaxFileBytes / block.size(); ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    const splat::Result<vfs::LocationStore> sized = vfs::LocationStore::open(file);
    REQUIRE_FALSE(sized.ok());
    CHECK(sized.message().find("byte limit") != std::string::npos);

    // A "locations" value that is not an array is refused by name, not walked.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"locations\":{\"smb0\":{\"host\":\"h\"}}}";
    }
    const splat::Result<vfs::LocationStore> shaped = vfs::LocationStore::open(file);
    REQUIRE_FALSE(shaped.ok());
    CHECK(shaped.message().find("not an array") != std::string::npos);
}

TEST_CASE("mount_all reports one line per location and registers what mounted") {
    splat_test::TempDir dir("vfs-locations-mount");
    splat::Result<vfs::LocationStore> opened = vfs::LocationStore::open(dir.path() / "l.json");
    REQUIRE(opened.ok());
    vfs::LocationStore store = opened.take();
    // A closed local port: the mount probe answers "offline" at once instead
    // of waiting on a name that does not resolve, and the line names the
    // location - the one string shape the Locations screen also shows.
    vfs::Location nfs;
    nfs.protocol = vfs::LocationProtocol::nfs;
    nfs.host = "127.0.0.1";
    nfs.port = 1;
    nfs.share = "/export";
    REQUIRE(store.add(nfs).ok());

    vfs::FileSystems file_systems;
    const std::vector<std::string> lines = store.mount_all(file_systems);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].compare(0, 15, "nfs0: offline: ") == 0);
    // The id appears once: mount_one()'s reason already names the location, so
    // prefixing it again read "nfs0: offline: nfs0: ...".
    CHECK(lines[0].find("nfs0: offline: nfs0:") == std::string::npos);
    CHECK(lines[0].find("nfs0") == lines[0].rfind("nfs0"));
    // A failed probe keeps the adapter: it was registered before the probe, so
    // a location path can never read as "unknown location" just because the
    // probe timed out (the console's resumed upload did exactly that). The
    // Locations screen still says "offline" - that is the probe's answer, not
    // the registry's.
    REQUIRE(file_systems.find("nfs0") != nullptr);
    // The store owns the id (add() assigns it to its copy), so the single
    // mount goes through the stored location, the way the UI's worker does.
    const vfs::Location* stored = store.find("nfs0");
    REQUIRE(stored != nullptr);
    CHECK(vfs::LocationStore::mount_one(file_systems, *stored).compare(0, 15, "offline: nfs0: ") ==
          0);
}

TEST_CASE("every configured location resolves by id before the first probe") {
    splat_test::TempDir dir("vfs-locations-register");
    splat::Result<vfs::LocationStore> opened = vfs::LocationStore::open(dir.path() / "l.json");
    REQUIRE(opened.ok());
    vfs::LocationStore store = opened.take();

    // Port 1 on loopback answers nothing: the adapter exists but its first
    // operation fails. That is the state a boot-time job is in while the
    // mount worker is still waiting for the NAS.
    vfs::Location share;
    share.host = "127.0.0.1";
    share.port = 1;
    share.share = "Photos";
    REQUIRE(store.add(share).ok());

    // The registry the vfs:: free functions route through (the app's one).
    auto& registry = vfs::file_systems();
    struct Cleanup {
        ~Cleanup() { vfs::file_systems().remove("smb0"); }
    } cleanup;

    const std::vector<std::string> failed = store.register_adapters(registry);
    CHECK(failed.empty());
    REQUIRE(registry.find("smb0") != nullptr);

    // No probe ran: registration is the cheap part and must not touch the
    // network. The proof is that the path now routes to the adapter and fails
    // with the adapter's own message instead of "unknown location".
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = vfs::list(vfs::join("smb0", ""), &entries);
    CHECK_FALSE(listed.ok());
    CHECK(listed.message().find("unknown location") == std::string::npos);
    CHECK(listed.message().find("smb0") != std::string::npos);

    // The probe is a second step and leaves the adapter where it is, so the
    // next use retries the same connection instead of reporting unknown.
    const vfs::Location* stored = store.find("smb0");
    REQUIRE(stored != nullptr);
    CHECK(vfs::LocationStore::mount_one(registry, *stored).compare(0, 14, "offline: smb0:") == 0);
    CHECK(registry.find("smb0") != nullptr);

    // Idempotent: a second pass must not replace the adapter that is in use.
    const std::shared_ptr<vfs::IFileSystem> before = registry.find("smb0");
    CHECK(store.register_adapters(registry).empty());
    CHECK(registry.find("smb0") == before);
}

namespace {

// A location that failed once: it refuses every operation until
// reset_connection() is called, exactly like an adapter whose connect failed and
// latched the error (console pass 3, L1).
class FailsOnceFileSystem : public splat::vfs::IFileSystem {
public:
    explicit FailsOnceFileSystem(std::string id) : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string describe() const override { return id_ + ":/"; }
    bool writable() const override { return true; }

    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        if (failed) return splat::Status::error("smb0: cannot connect: connect timed out");
        // The first attempt fails (the NAS is still booting) and latches, the
        // way a failed connect does; only reset_connection() clears the latch.
        if (!attempted) {
            attempted = true;
            failed = true;
            return splat::Status::error("smb0: cannot connect: connect timed out");
        }
        out->exists = true;
        out->directory = relative.empty();
        return splat::Status::success();
    }

    splat::Result<std::vector<uint8_t>> read(const std::string&, size_t) override {
        return splat::Result<std::vector<uint8_t>>::failure("fake");
    }
    splat::Status write(const std::string&, const uint8_t*, size_t) override {
        return splat::Status::success();
    }
    splat::Status list(const std::string&, std::vector<splat::vfs::DirEntry>*) override {
        return failed ? splat::Status::error("fake: offline") : splat::Status::success();
    }
    splat::Status make_dirs(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_file(const std::string&) override { return splat::Status::success(); }
    splat::Status remove_tree(const std::string&) override { return splat::Status::success(); }

    void reset_connection() override { failed = false; }
    std::string connection_error() const override {
        return failed ? std::string("smb0: cannot connect: connect timed out") : std::string();
    }

    bool failed = false;
    bool attempted = false;

private:
    std::string id_;
};

}  // namespace

TEST_CASE("a location that failed once retries on the next mount (L1)") {
    // The screen's Test is LocationStore::mount_one, and the boot mount is the
    // same call. Both used to reuse the adapter's latched error, so a NAS that
    // was still booting when the app started stayed offline for the life of the
    // process. mount_one resets the adapter first; this fake refuses until
    // reset_connection() is called, so the second mount can only say "ready"
    // because the reset happened.
    splat::vfs::FileSystems file_systems;
    FailsOnceFileSystem* adapter = new FailsOnceFileSystem("smb0");
    std::shared_ptr<splat::vfs::IFileSystem> owned(adapter);
    file_systems.add(owned);

    splat::vfs::Location location;
    location.id = "smb0";
    location.host = "192.0.2.63";
    location.share = "Photos";

    const std::string first = splat::vfs::LocationStore::mount_one(file_systems, location);
    CHECK(first.compare(0, 8, "offline:") == 0);
    CHECK(first.find("connect timed out") != std::string::npos);

    // The same adapter, mounted again: this is the Test button.
    const std::string second = splat::vfs::LocationStore::mount_one(file_systems, location);
    CHECK(second == "ready");

    file_systems.remove("smb0");
}
