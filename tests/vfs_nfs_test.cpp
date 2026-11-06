// core/vfs/nfs: the URL building (v3 with a custom port, v4 without a mount
// port, the default ports left out, uid/gid from the location) and the offline
// path - a mount of 127.0.0.1:1 fails inside the timeout with a message that
// names the location. There is no password anywhere: NFS has none.
//
// The live test is tagged [nfs-live] and runs only when SPLAT_NFS_URL is set
// (Appendix C: anyvm-org/nfsd in user space, no root, no rpcbind).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <map>
#include <string>

#include "common/path.h"
#include "vfs/io.h"
#include "vfs/location.h"
#include "vfs/nfs.h"
#include "vfs/net_state.h"
#include "vfs/resolve.h"

namespace vfs = splat::vfs;

namespace {

// The console's process has no name service (console pass 3, P1): every live
// case runs with the resolver switched off, so the numeric path the title uses
// is the path under test here too.
struct NoNameResolver {
    std::string previous;
    bool had = false;

    NoNameResolver() {
        const char* value = std::getenv("SPLAT_RESOLVE_FORCE_FAIL");
        had = value != nullptr;
        if (had) previous = value;
        setenv("SPLAT_RESOLVE_FORCE_FAIL", "1", 1);
    }
    ~NoNameResolver() {
        if (had) {
            setenv("SPLAT_RESOLVE_FORCE_FAIL", previous.c_str(), 1);
        } else {
            unsetenv("SPLAT_RESOLVE_FORCE_FAIL");
        }
    }
};

vfs::Location location() {
    vfs::Location out;
    out.id = "nfs0";
    out.protocol = vfs::LocationProtocol::nfs;
    out.host = "nas.local";
    out.share = "/export";
    return out;
}

// nfs://host/export[/path][?k=v][&k=v]
struct ParsedUrl {
    std::string host;
    std::string export_path;
    std::string path;
    std::map<std::string, std::string> query;
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl out;
    std::string rest = url;
    if (rest.compare(0, 6, "nfs://") == 0) rest.erase(0, 6);
    const std::size_t question = rest.find('?');
    std::string query;
    if (question != std::string::npos) {
        query = rest.substr(question + 1);
        rest.erase(question);
    }
    const std::size_t slash = rest.find('/');
    out.host = rest.substr(0, slash);
    const std::string tail = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
    const std::size_t first = tail.find('/');
    out.export_path = "/" + tail.substr(0, first);
    if (first != std::string::npos) out.path = tail.substr(first + 1);

    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair =
            query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) {
            out.query[pair.substr(0, equals)] = pair.substr(equals + 1);
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

}  // namespace

TEST_CASE("nfs_url builds NFSv3 with a custom port and NFSv4 without a mount port") {
    vfs::Location v3 = location();
    v3.port = 2049;
    CHECK(vfs::nfs_url(v3) ==
          "nfs://nas.local/export?version=3&nfsport=2049&mountport=2049&uid=65534&gid=65534");

    vfs::Location with_path = v3;
    with_path.path = "/scenes/2026/";
    CHECK(vfs::nfs_url(with_path) ==
          "nfs://nas.local/export/scenes/2026?version=3&nfsport=2049&mountport=2049"
          "&uid=65534&gid=65534");

    vfs::Location v4 = location();
    v4.port = 2049;
    v4.nfs_version = 4;
    // NFSv4 has no MOUNT program, so no mountport in the URL.
    CHECK(vfs::nfs_url(v4) == "nfs://nas.local/export?version=4&nfsport=2049&uid=65534&gid=65534");
}

TEST_CASE("nfs_url leaves the default ports out and carries the location's uid/gid") {
    vfs::Location defaults = location();
    CHECK(vfs::nfs_url(defaults) == "nfs://nas.local/export?version=3&uid=65534&gid=65534");

    vfs::Location named = defaults;
    named.uid = 1000;
    named.gid = 1001;
    CHECK(vfs::nfs_url(named) == "nfs://nas.local/export?version=3&uid=1000&gid=1001");
}

TEST_CASE("nfs_url never carries a password") {
    // NFS has no password field in the URL or the protocol: AUTH_SYS is the
    // uid/gid, and a password typed on an SMB row must not leak into the NFS
    // adapter's description after the protocol is switched.
    vfs::Location switched = location();
    switched.password = "hunter2";
    switched.user = "test";
    const std::string url = vfs::nfs_url(switched);
    CHECK(url.find("hunter2") == std::string::npos);
    CHECK(url.find("test") == std::string::npos);
}

TEST_CASE("an NFS mount of a closed port fails inside the timeout, naming the location") {
    vfs::Location broken = location();
    broken.host = "127.0.0.1";
    broken.share = "/export";
    broken.port = 1;

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_nfs_file_system(broken, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);

    const auto started = std::chrono::steady_clock::now();
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    CHECK_FALSE(listed.ok());
    CHECK(seconds < 7.0);
    CHECK(listed.message().find("nfs0") != std::string::npos);
}

TEST_CASE("an incomplete NFS location is refused by name") {
    vfs::Location broken = location();
    broken.share.clear();
    std::string error;
    CHECK(vfs::make_nfs_file_system(broken, &error) == nullptr);
    CHECK(error.find("nfs0") != std::string::npos);
}

TEST_CASE("an NFS export named by host name fails without a resolver, and says why") {
    NoNameResolver no_names;
    vfs::Location named = location();
    named.host = "nas.local";
    named.port = 2049;

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_nfs_file_system(named, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_FALSE(listed.ok());
    MESSAGE("an NFS name with no resolver: " << listed.message());
    CHECK(listed.message().find("nfs0") != std::string::npos);
    CHECK(listed.message().find("nas.local") != std::string::npos);
    CHECK(listed.message().find("cannot resolve") != std::string::npos);
    CHECK(listed.message().find("EAI_FAIL") != std::string::npos);
    CHECK(listed.message().find("IP address") != std::string::npos);
}

TEST_CASE("a dead network stack is named as the stack, not as the export") {
    // P2 on the console: sceNetInit() failed, so the export was never
    // contacted. "cannot mount" would blame a server that is fine.
    vfs::set_net_stack_error(
        "sceNetInit -> -1 (0x8000000a); the console network stack is unavailable, so no "
        "location can connect");
    struct Clear {
        ~Clear() { vfs::set_net_stack_error(std::string()); }
    } cleared;

    vfs::Location dead = location();
    dead.host = "127.0.0.1";
    dead.port = 1;
    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_nfs_file_system(dead, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_FALSE(listed.ok());
    MESSAGE("dead stack: " << listed.message());
    CHECK(listed.message().find("no network stack") != std::string::npos);
    CHECK(listed.message().find("sceNetInit") != std::string::npos);
    CHECK(listed.message().find("cannot mount") == std::string::npos);
    CHECK(mounted->connection_error().find("network stack") != std::string::npos);

    // L1: once the stack is up the same adapter is retried.
    vfs::set_net_stack_error(std::string());
    mounted->reset_connection();
    std::vector<vfs::DirEntry> retried_entries;
    const splat::Status retried = mounted->list("", &retried_entries);
    REQUIRE_FALSE(retried.ok());
    CHECK(retried.message().find("no network stack") == std::string::npos);
}

TEST_CASE("a live NFS export lists, reads, writes and unlinks [nfs-live]") {
    const char* url = std::getenv("SPLAT_NFS_URL");
    if (url == nullptr || url[0] == '\0') {
        MESSAGE("SPLAT_NFS_URL is not set: the live NFS check is skipped");
        return;
    }
    NoNameResolver no_names;
    const ParsedUrl parsed = parse_url(url);
    REQUIRE_FALSE(parsed.host.empty());
    REQUIRE_FALSE(parsed.export_path.empty());
    // With the resolver off only a literal can mount: the case would otherwise
    // pass on this Mac's working resolver without touching the title's path.
    vfs::NumericAddress literal;
    REQUIRE_MESSAGE(vfs::parse_numeric_host(parsed.host, &literal),
                    "set SPLAT_NFS_URL to an IP literal: a name cannot resolve without a resolver");

    vfs::Location live;
    live.id = "nfs0";
    live.protocol = vfs::LocationProtocol::nfs;
    live.host = parsed.host;
    live.share = parsed.export_path;
    live.path = parsed.path;
    live.nfs_version =
        parsed.query.count("version") != 0 ? std::atoi(parsed.query.at("version").c_str()) : 3;
    live.port = parsed.query.count("nfsport") != 0
                    ? std::atoi(parsed.query.at("nfsport").c_str())
                    : 0;
    live.uid = parsed.query.count("uid") != 0
                   ? static_cast<uint32_t>(std::atoi(parsed.query.at("uid").c_str()))
                   : 65534u;
    live.gid = parsed.query.count("gid") != 0
                   ? static_cast<uint32_t>(std::atoi(parsed.query.at("gid").c_str()))
                   : 65534u;

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_nfs_file_system(live, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);

    // The export root lists.
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_MESSAGE(listed.ok(), listed.message());
    MESSAGE("live NFS root: " << entries.size() << " entries");

    const std::string name = "splat-vfs-live.txt";
    const std::string text = "hello from the splat NFS VFS";
    const splat::Status written =
        mounted->write(name, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    REQUIRE_MESSAGE(written.ok(), written.message());

    const splat::Result<std::vector<uint8_t>> back = mounted->read(name, 0);
    REQUIRE_MESSAGE(back.ok(), back.message());
    CHECK(std::string(back.value().begin(), back.value().end()) == text);

    vfs::FileStat status;
    REQUIRE(mounted->stat(name, &status).ok());
    CHECK(status.exists);
    CHECK_FALSE(status.directory);
    CHECK(status.size == text.size());

    // A rooted read can be repeated: the file is on the server, not cached here.
    const splat::Result<std::vector<uint8_t>> again = mounted->read(name, 0);
    REQUIRE_MESSAGE(again.ok(), again.message());
    CHECK(again.value().size() == text.size());

    REQUIRE(mounted->remove_file(name).ok());
    REQUIRE(mounted->stat(name, &status).ok());
    CHECK_FALSE(status.exists);
}
