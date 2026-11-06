// core/vfs/smb: the URL building (guest, user, custom port, seal), and the
// offline path - a connect to 127.0.0.1:1 fails inside the timeout with a
// message that names the location and never the password.
//
// The live test is tagged [smb-live] and runs only when SPLAT_SMB_URL is set
// (Appendix C: impacket's smbserver on port 1445).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/path.h"
#include "scene/scene_folder.h"
#include "vfs/io.h"
#include "vfs/location.h"
#include "vfs/net_state.h"
#include "vfs/resolve.h"
#include "vfs/smb.h"

namespace vfs = splat::vfs;
using splat::Path;

namespace {

// The console's process has no name service: getaddrinfo is a stub that returns
// EAI_FAIL for every input (console pass 3, P1). Every live case runs with the
// resolver switched off so the numeric path the title uses is the path the test
// exercises, and a name that this Mac could resolve fails here exactly as it
// fails there.
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
    out.id = "smb0";
    out.host = "nas.local";
    out.share = "Photos";
    return out;
}

// smb://[user[:password]@]host[:port]/share[/path][?k=v][&k=v]
struct ParsedUrl {
    std::string host;
    int port = 0;
    std::string share;
    std::string path;
    std::map<std::string, std::string> query;
};

ParsedUrl parse_url(const std::string& url) {
    ParsedUrl out;
    std::string rest = url;
    if (rest.compare(0, 6, "smb://") == 0) rest.erase(0, 6);
    const std::size_t hash = rest.find('#');
    if (hash != std::string::npos) rest.erase(hash);
    const std::size_t question = rest.find('?');
    std::string query;
    if (question != std::string::npos) {
        query = rest.substr(question + 1);
        rest.erase(question);
    }
    const std::size_t at = rest.rfind('@');
    if (at != std::string::npos) rest.erase(0, at + 1);  // user:password@ dropped

    const std::size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    const std::string tail = slash == std::string::npos ? std::string() : rest.substr(slash + 1);

    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.c_str() + colon + 1);
    } else {
        out.host = authority;
    }
    const std::size_t first = tail.find('/');
    out.share = tail.substr(0, first);
    if (first != std::string::npos) out.path = tail.substr(first + 1);

    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair = query.substr(start, amp == std::string::npos
                                                         ? std::string::npos
                                                         : amp - start);
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

TEST_CASE("smb_url builds the four shapes and never a password") {
    vfs::Location guest = location();
    CHECK(vfs::smb_url(guest) == "smb://nas.local/Photos");

    vfs::Location user = location();
    user.user = "test";
    user.password = "secret";
    CHECK(vfs::smb_url(user) == "smb://test@nas.local/Photos");
    CHECK(vfs::smb_url(user).find("secret") == std::string::npos);

    vfs::Location port = location();
    port.port = 1445;
    CHECK(vfs::smb_url(port) == "smb://nas.local:1445/Photos");

    vfs::Location sealed = location();
    sealed.encrypt = true;
    CHECK(vfs::smb_url(sealed) == "smb://nas.local/Photos?seal");

    vfs::Location path = location();
    path.port = 1445;
    path.path = "/scenes/2026/";
    CHECK(vfs::smb_url(path) == "smb://nas.local:1445/Photos/scenes/2026");
}

TEST_CASE("the SMB connect host carries a non-default port") {
    // libsmb2 6.3 has no smb2_set_port(): the port is a suffix on the server
    // string, split before getaddrinfo (lib/socket.c). Port 0 and the default
    // 445 leave it off; anything else has to be there or a test server on 1445
    // is never reached (the console pass's impacket share).
    vfs::Location port = location();
    CHECK(vfs::smb_connect_host(port) == "nas.local");
    port.port = 445;
    CHECK(vfs::smb_connect_host(port) == "nas.local");
    port.port = 1445;
    CHECK(vfs::smb_connect_host(port) == "nas.local:1445");
    port.port = 139;
    CHECK(vfs::smb_connect_host(port) == "nas.local:139");
    CHECK(vfs::smb_url(port).find(":139/") != std::string::npos);
}

TEST_CASE("an IPv6 SMB host is bracketed so the port suffix still splits") {
    // Location::host's own example is "::1", and libsmb2 splits the server
    // string at the first ':' (after an optional "[...]" prefix), so an
    // unbracketed literal becomes host "" and port ":1" - a share that never
    // connects. The brackets are the address grammar libsmb2 expects.
    vfs::Location v6 = location();
    v6.host = "::1";
    CHECK(vfs::smb_connect_host(v6) == "[::1]");
    CHECK(vfs::smb_url(v6) == "smb://[::1]/Photos");

    v6.port = 1445;
    CHECK(vfs::smb_connect_host(v6) == "[::1]:1445");
    CHECK(vfs::smb_url(v6) == "smb://[::1]:1445/Photos");

    // A location written by hand with the brackets already there is unchanged.
    v6.host = "[fe80::1]";
    CHECK(vfs::smb_connect_host(v6) == "[fe80::1]:1445");
    v6.port = 0;
    CHECK(vfs::smb_connect_host(v6) == "[fe80::1]");
}

TEST_CASE("an SMB connect to a closed port fails inside the timeout, without the password") {
    vfs::Location broken = location();
    broken.host = "127.0.0.1";
    broken.port = 1;
    broken.user = "test";
    broken.password = "hunter2";

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(broken, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);

    const auto started = std::chrono::steady_clock::now();
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    CHECK_FALSE(listed.ok());
    CHECK(seconds < 7.0);
    CHECK(listed.message().find("smb0") != std::string::npos);
    CHECK(listed.message().find("hunter2") == std::string::npos);
}

TEST_CASE("an incomplete SMB location is refused by name") {
    vfs::Location broken = location();
    broken.share.clear();
    std::string error;
    CHECK(vfs::make_smb_file_system(broken, &error) == nullptr);
    CHECK(error.find("smb0") != std::string::npos);
}

TEST_CASE("a host name with no resolver is named as a resolver failure, not a bad address") {
    // P1/P2: this process's getaddrinfo answers EAI_FAIL for everything, so a
    // name never reaches the share. The message must say that, and name the
    // failure symbolically (EAI_FAIL, not the platform's number), instead of
    // libsmb2's "Invalid address" (which reads like a typo in a correct
    // address) and instead of "cannot connect" (which blames the server).
    NoNameResolver no_names;
    vfs::Location named = location();
    named.host = "nas.local";
    named.port = 1445;
    named.user = "test";
    named.password = "hunter2";

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(named, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_FALSE(listed.ok());
    MESSAGE("a name with no resolver: " << listed.message());
    CHECK(listed.message().find("nas.local") != std::string::npos);
    CHECK(listed.message().find("cannot resolve") != std::string::npos);
    CHECK(listed.message().find("EAI_FAIL") != std::string::npos);
    CHECK(listed.message().find("IP address") != std::string::npos);
    CHECK(listed.message().find("Invalid address") == std::string::npos);
    CHECK(listed.message().find("hunter2") == std::string::npos);
    CHECK(entries.empty());
}

TEST_CASE("a refused login reads as wrong user name or password") {
    // BUG 3: a conformant server's answer and the one libsmb2 cannot decode
    // (impacket 0.11.0 writes a 9-byte error body) both have to read as the
    // credentials, in words a user can act on, and never carry the password.
    vfs::Location live = location();
    live.host = "192.0.2.63";
    live.port = 1445;
    live.user = "splat-test";
    live.password = "hunter2";
    const std::string server = vfs::smb_connect_host(live);

    for (const char* library : {
             "smb2_service failed with : Failed to parse fixed part of command payload. "
             "Error data extends beyond end of PDU",
             "Session setup failed with (0xc000006d) STATUS_LOGON_FAILURE",
         }) {
        const std::string message =
            vfs::smb_connect_error(live, server, std::string(), library);
        MESSAGE("refused login: " << message);
        CHECK(message.find("smb0") != std::string::npos);
        CHECK(message.find("splat-test") != std::string::npos);
        CHECK(message.find("wrong user name or password") != std::string::npos);
        CHECK(message.find("hunter2") == std::string::npos);
    }

    // Anything else keeps the library's own text, and the connect target: a
    // stopped server has to read as a stopped server, not as bad credentials.
    const std::string refused = vfs::smb_connect_error(
        live, server, std::string(), "Connect failed with errno : Connection refused(61)");
    CHECK(refused.find("wrong user name") == std::string::npos);
    CHECK(refused.find("Connection refused") != std::string::npos);
    CHECK(refused.find("192.0.2.63:1445/Photos") != std::string::npos);

    const std::string empty = vfs::smb_connect_error(live, server, std::string(), "");
    CHECK(empty == "smb0: cannot connect to 192.0.2.63:1445/Photos");

    // A resolver failure wins over both: the share was never contacted.
    const std::string unresolved =
        vfs::smb_connect_error(live, server, vfs::resolve_name_error("nas.local"),
                               "Invalid address:nas.local:1445  Can not resolve into IPv4/v6.");
    CHECK(unresolved.find("cannot resolve") != std::string::npos);
    CHECK(unresolved.find("enter the server's IP address") != std::string::npos);
    CHECK(unresolved.find("Invalid address") == std::string::npos);
}

TEST_CASE("a live SMB share lists, reads, writes and unlinks [smb-live]") {
    const char* url = std::getenv("SPLAT_SMB_URL");
    if (url == nullptr || url[0] == '\0') {
        MESSAGE("SPLAT_SMB_URL is not set: the live SMB check is skipped");
        return;
    }
    NoNameResolver no_names;
    const ParsedUrl parsed = parse_url(url);
    REQUIRE_FALSE(parsed.host.empty());
    REQUIRE_FALSE(parsed.share.empty());
    // The only host that can work with the resolver off is a literal: this is
    // the assertion that keeps the case honest on a Mac whose resolver works.
    vfs::NumericAddress literal;
    REQUIRE_MESSAGE(vfs::parse_numeric_host(parsed.host, &literal),
                    "set SPLAT_SMB_URL to an IP literal: a name cannot resolve without a resolver");

    vfs::Location live;
    live.id = "smb0";
    live.host = parsed.host;
    live.port = parsed.port;
    live.share = parsed.share;
    live.path = parsed.path;
    live.user = parsed.query.count("user") != 0 ? parsed.query.at("user") : std::string();
    live.password =
        parsed.query.count("password") != 0 ? parsed.query.at("password") : std::string();

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(live, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_MESSAGE(listed.ok(), listed.message());
    MESSAGE("live SMB root: " << entries.size() << " entries");

    const std::string name = "splat-vfs-live.txt";
    const std::string text = "hello from the splat VFS";
    REQUIRE(mounted->write(name, reinterpret_cast<const uint8_t*>(text.data()), text.size()).ok());

    const splat::Result<std::vector<uint8_t>> back = mounted->read(name, 0);
    REQUIRE_MESSAGE(back.ok(), back.message());
    CHECK(std::string(back.value().begin(), back.value().end()) == text);

    vfs::FileStat status;
    REQUIRE(mounted->stat(name, &status).ok());
    CHECK(status.exists);
    CHECK(status.size == text.size());

    // A name that is not there is a normal answer, and it must not poison the
    // next call (mac-share/PROBE.md: nterror is stale, the -errno is not).
    vfs::FileStat absent;
    REQUIRE(mounted->stat("splat-vfs-absent.bin", &absent).ok());
    CHECK_FALSE(absent.exists);
    vfs::FileStat after_absent;
    REQUIRE(mounted->stat(name, &after_absent).ok());
    CHECK(after_absent.exists);
    CHECK(after_absent.size == text.size());
    const splat::Result<std::vector<uint8_t>> still_there = mounted->read(name, 0);
    REQUIRE_MESSAGE(still_there.ok(), still_there.message());
    CHECK(std::string(still_there.value().begin(), still_there.value().end()) == text);

    REQUIRE(mounted->remove_file(name).ok());
}

// Stage P2's replace path over the share: rotation and a photo turn publish
// through vfs::replace_file / vfs::rename_file, and SMB's rename cannot replace
// an existing name, so rename_file() swaps through a sidecar backup
// (IFileSystem::rename_replaces() == false). Nothing exercised that on an
// smb0: path before.
TEST_CASE("a live SMB share replaces and renames a file through the VFS [smb-live]") {
    const char* url = std::getenv("SPLAT_SMB_URL");
    if (url == nullptr || url[0] == '\0') {
        MESSAGE("SPLAT_SMB_URL is not set: the live SMB replace check is skipped");
        return;
    }
    NoNameResolver no_names;
    const ParsedUrl parsed = parse_url(url);
    REQUIRE_FALSE(parsed.host.empty());
    REQUIRE_FALSE(parsed.share.empty());
    vfs::NumericAddress literal;
    REQUIRE_MESSAGE(vfs::parse_numeric_host(parsed.host, &literal),
                    "set SPLAT_SMB_URL to an IP literal: a name cannot resolve without a resolver");

    vfs::Location live;
    live.id = "smb0";
    live.host = parsed.host;
    live.port = parsed.port;
    live.share = parsed.share;
    live.path = parsed.path;
    live.user = parsed.query.count("user") != 0 ? parsed.query.at("user") : std::string();
    live.password =
        parsed.query.count("password") != 0 ? parsed.query.at("password") : std::string();

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(live, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    struct Registered {
        std::shared_ptr<vfs::IFileSystem> share;
        ~Registered() { vfs::file_systems().remove(share->id()); }
    } registered{mounted};
    vfs::file_systems().add(mounted);

    const splat::Path source = vfs::join("smb0", "splat-vfs-replace-source.txt");
    const splat::Path target = vfs::join("smb0", "splat-vfs-replace-target.txt");
    (void)vfs::remove_file(source);
    (void)vfs::remove_file(target);

    const std::string first = "the first version";
    const std::string second = "the second version, longer than the first";
    REQUIRE(splat::vfs::write_file(source, reinterpret_cast<const uint8_t*>(first.data()),
                                    first.size())
                .ok());

    // replace_file: write beside, then rename over. SMB refuses a rename onto
    // an existing name, so this is the sidecar path.
    REQUIRE_MESSAGE(splat::vfs::replace_file(source, reinterpret_cast<const uint8_t*>(second.data()),
                                                  second.size())
                        .ok(),
                    "replace_file on the share");
    const splat::Result<std::vector<uint8_t>> replaced = mounted->read("splat-vfs-replace-source.txt", 0);
    REQUIRE_MESSAGE(replaced.ok(), replaced.message());
    CHECK(std::string(replaced.value().begin(), replaced.value().end()) == second);

    // rename_file onto a name that already exists: the swap path too.
    REQUIRE(splat::vfs::write_file(target, reinterpret_cast<const uint8_t*>(first.data()),
                                    first.size())
                .ok());
    REQUIRE_MESSAGE(splat::vfs::rename_file(source, target).ok(), "rename_file onto an existing name");
    const splat::Result<std::vector<uint8_t>> renamed = mounted->read("splat-vfs-replace-target.txt", 0);
    REQUIRE_MESSAGE(renamed.ok(), renamed.message());
    CHECK(std::string(renamed.value().begin(), renamed.value().end()) == second);
    vfs::FileStat gone;
    REQUIRE(mounted->stat("splat-vfs-replace-source.txt", &gone).ok());
    CHECK_FALSE(gone.exists);

    REQUIRE(splat::vfs::remove_file(target).ok());
}

// A3.3/A3.4/A3.6 without the console: a four-file scene folder completed
// locally, copied to the share, verified file by file (the runner's upload
// step), the staging copy removed, and a loose splat file read back from the
// share through read_scene_path - the "open a .spz in place" path. The one
// timing the checklist asks for (one 40 MB read) is measured here too; a
// live run prints it as a MESSAGE line.
TEST_CASE("a live SMB share takes a four-file scene, deletes the staging copy and "
          "reads a splat in place [smb-live]") {
    const char* url = std::getenv("SPLAT_SMB_URL");
    if (url == nullptr || url[0] == '\0') {
        MESSAGE("SPLAT_SMB_URL is not set: the live upload check is skipped");
        return;
    }
    NoNameResolver no_names;
    const ParsedUrl parsed = parse_url(url);
    REQUIRE_FALSE(parsed.host.empty());
    REQUIRE_FALSE(parsed.share.empty());
    vfs::NumericAddress literal;
    REQUIRE_MESSAGE(vfs::parse_numeric_host(parsed.host, &literal),
                    "set SPLAT_SMB_URL to an IP literal: a name cannot resolve without a resolver");

    vfs::Location live;
    live.id = "smb0";
    live.host = parsed.host;
    live.port = parsed.port;
    live.share = parsed.share;
    live.path = parsed.path;
    live.user = parsed.query.count("user") != 0 ? parsed.query.at("user") : std::string();
    live.password =
        parsed.query.count("password") != 0 ? parsed.query.at("password") : std::string();

    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(live, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    // Register it the way the app does (LocationStore::mount_one): copy_tree
    // and read_scene_path go through the one registry, so a bare local
    // `mounted` handle cannot reach `smb0:/...`. The destructor removes it
    // again even when an assertion below aborts the case.
    struct Registered {
        std::shared_ptr<vfs::IFileSystem> share;
        ~Registered() { vfs::file_systems().remove(share->id()); }
    } registered{mounted};
    vfs::file_systems().add(mounted);

    // ---- connect/list timing (A3's "record timings") ----------------------
    const auto mounted_at = std::chrono::steady_clock::now();
    vfs::FileStat share_root;
    const splat::Status root_status = mounted->stat("", &share_root);
    REQUIRE_MESSAGE(root_status.ok(), root_status.message());
    std::vector<vfs::DirEntry> root;
    const splat::Status listed_root = mounted->list("", &root);
    REQUIRE_MESSAGE(listed_root.ok(), listed_root.message());

    // ---- stage a scene folder with the four files ------------------------
    const splat::Path staging = std::filesystem::temp_directory_path() / "splat-smb-upload";
    std::filesystem::remove_all(staging);
    REQUIRE(std::filesystem::create_directories(staging));
    // scene.psplat is 40 MiB, about 1.5 M splats at 28 B each, i.e. a realistic
    // scene rather than a placeholder (A3 asks for a 40 MB read). The other
    // three are the folder's real companions.
    using File = std::pair<std::string, std::vector<uint8_t>>;
    std::vector<File> four_files;
    {
        std::vector<uint8_t> splat(40u * 1024u * 1024u, 0);
        for (std::size_t i = 0; i < splat.size(); i += 4096) {
            splat[i] = static_cast<uint8_t>((i / 4096) & 0xFF);
        }
        four_files.emplace_back("scene.psplat", std::move(splat));
        four_files.emplace_back("meta.json", std::vector<uint8_t>{'{', '}', '\n'});
        four_files.emplace_back("thumb.jpg", std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xD9});
        four_files.emplace_back("original.jpg", std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xD9});
    }
    for (const File& file : four_files) {
        const splat::Status written = splat::vfs::write_file(staging / file.first,
                                                            file.second.data(), file.second.size());
        REQUIRE_MESSAGE(written.ok(), written.message());
    }

    // One top-level folder, so the case leaves nothing behind in the share:
    // a nested "scenes/live-scene" leaves its empty parent after the cleanup.
    const std::string relative = "splat-vfs-live-scene";
    // Start from a clean folder: a half-written scene from an earlier run
    // makes impacket's server answer a stale directory and the copy fails with
    // an obscure libsmb2 error ("No io vectors left to read into").
    (void)splat::vfs::remove_tree(splat::vfs::join("smb0", relative));
    const splat::Status copied = splat::vfs::copy_tree(staging, splat::vfs::join("smb0", relative));
    REQUIRE_MESSAGE(copied.ok(), copied.message());

    // ---- every file arrived whole, as the runner checks ---------------
    for (const File& file : four_files) {
        vfs::FileStat on_share;
        const splat::Status stat = mounted->stat(relative + "/" + file.first, &on_share);
        REQUIRE_MESSAGE(stat.ok(), stat.message());
        CHECK(on_share.exists);
        CHECK(on_share.size == file.second.size());
    }
    const double connect_list_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - mounted_at).count();
    MESSAGE("live SMB: mount + list + 4-file upload " << connect_list_seconds << " s");

    // A folder that already exists (smb-e2e, BUG 2): make_dirs asks before it
    // creates, so the second call costs no failed mkdir, and the connection that
    // made the first upload still lists.
    REQUIRE_MESSAGE(mounted->make_dirs(relative).ok(), "make_dirs on an existing folder");
    std::vector<vfs::DirEntry> after_make_dirs;
    REQUIRE_MESSAGE(mounted->list(relative, &after_make_dirs).ok(),
                    "the connection died on a folder that already existed");
    CHECK(after_make_dirs.size() == four_files.size());

    // One error reply (a name that is not there) must not break the adapter
    // (smb-e2e, BUG 1): the read after it still works, on a rebuilt connection.
    const splat::Result<std::vector<uint8_t>> missing =
        mounted->read("does-not-exist.bin", 0);
    CHECK_FALSE(missing.ok());
    const splat::Result<std::vector<uint8_t>> recovered =
        mounted->read(relative + "/meta.json", 0);
    REQUIRE_MESSAGE(recovered.ok(), "the location broke after one error reply: "
                                        << recovered.message());
    CHECK(recovered.value().size() == 3u);

    // ---- the staging copy goes, which is what "tmp empty" means ----------
    std::filesystem::remove_all(staging);
    CHECK_FALSE(std::filesystem::exists(staging));

    // ---- one 40 MB read, timed (A3) --------------------------------------
    const auto read_at = std::chrono::steady_clock::now();
    const splat::Result<std::vector<uint8_t>> big =
        mounted->read(relative + "/scene.psplat", 0);
    REQUIRE_MESSAGE(big.ok(), big.message());
    const double read_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - read_at).count();
    CHECK(big.value().size() == four_files.front().second.size());
    MESSAGE("live SMB: 40 MB read " << read_seconds << " s ("
            << (40.0 / (read_seconds > 0.0 ? read_seconds : 1.0)) << " MB/s)");

    // ---- a loose .splat opened in place from the share -------------------
    // Two splats: position(3), scale(3), colour(4 as uint8), rotation(4), all
    // as the .splat reader expects; the point is that it is read from the
    // share, not that the numbers are interesting.
    std::vector<uint8_t> tiny(64, 0);
    for (int splat = 0; splat < 2; ++splat) {
        const std::size_t base = static_cast<std::size_t>(splat) * 32;
        tiny[base + 24 + 0] = 255;  // sh_dc r
        tiny[base + 24 + 3] = 255;  // alpha: opaque
        tiny[base + 28 + 3] = 255;  // rotation w
    }
    const splat::Status wrote_tiny =
        splat::vfs::write_file(splat::vfs::join("smb0", relative + "/tiny.splat"),
                               tiny.data(), tiny.size());
    REQUIRE_MESSAGE(wrote_tiny.ok(), wrote_tiny.message());
    const splat::Result<splat::LoadedScene> opened =
        splat::read_scene_path(splat::vfs::join("smb0", relative + "/tiny.splat"));
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    CHECK(opened.value().scene.count() == 2u);

    // ---- clean up the share ------------------------------------------------
    REQUIRE(splat::vfs::remove_tree(splat::vfs::join("smb0", relative)).ok());
}

TEST_CASE("a missing name is read from the status, never from a stale nterror") {
    // mac-share/PROBE.md: smb2_get_nterror() keeps the previous reply's
    // status, so reading it after a failed call can turn an access-denied
    // answer into "no such file". stat() of a denied path would then look like
    // an absent one, and unique_scene_folder() could publish over a folder that
    // really exists.
    CHECK(vfs::smb_missing_name(-ENOENT));
    CHECK_FALSE(vfs::smb_missing_name(-EACCES));
    CHECK_FALSE(vfs::smb_missing_name(-EIO));
    CHECK_FALSE(vfs::smb_missing_name(0));
}

TEST_CASE("a dead network stack is named as the stack, not as the share") {
    // P2 on the console: sceNetInit() failed, so the share was never
    // contacted. The location's line must say that; libsmb2's "cannot connect"
    // would blame a server that is fine.
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
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(dead, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);
    std::vector<vfs::DirEntry> entries;
    const splat::Status listed = mounted->list("", &entries);
    REQUIRE_FALSE(listed.ok());
    MESSAGE("dead stack: " << listed.message());
    CHECK(listed.message().find("no network stack") != std::string::npos);
    CHECK(listed.message().find("sceNetInit") != std::string::npos);
    CHECK(listed.message().find("cannot connect") == std::string::npos);
    // And the same sentence is what the Locations screen shows as its health.
    CHECK(mounted->connection_error().find("network stack") != std::string::npos);

    // L1: once the stack is up, the same adapter is retried and the location's
    // failure is the port, not the stack. The stack failure was not final.
    vfs::set_net_stack_error(std::string());
    mounted->reset_connection();
    std::vector<vfs::DirEntry> retried_entries;
    const splat::Status retried = mounted->list("", &retried_entries);
    REQUIRE_FALSE(retried.ok());
    MESSAGE("after the stack came back: " << retried.message());
    CHECK(retried.message().find("no network stack") == std::string::npos);
    CHECK(retried.message().find("cannot connect") != std::string::npos);
}

TEST_CASE("connection_error never waits for a connect that holds the adapter") {
    // SPEC 3.1: check_health() runs on the UI thread. It reads
    // IFileSystem::connection_error(), so that call must not take the lock a
    // listing or a connect holds - the old code took it and the panel froze for
    // the whole smb2_set_timeout(5).
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener, reinterpret_cast<struct sockaddr*>(&address), &length) == 0);
    const int port = ntohs(address.sin_port);

    vfs::Location stuck = location();
    stuck.host = "127.0.0.1";
    stuck.share = "Photos";
    stuck.port = port;
    std::string error;
    const std::shared_ptr<vfs::IFileSystem> mounted = vfs::make_smb_file_system(stuck, &error);
    REQUIRE_MESSAGE(mounted != nullptr, error);

    std::thread reader([&mounted] {
        std::vector<vfs::DirEntry> entries;
        (void)mounted->list("", &entries);
    });
    // Accept the connection the reader makes; the server then says nothing, so
    // libsmb2 waits for its 5 s read timeout with the adapter's lock held.
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);

    const auto asked_at = std::chrono::steady_clock::now();
    (void)mounted->connection_error();
    const double waited =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - asked_at).count();
    MESSAGE("connection_error while connecting: " << waited << " s");
    CHECK(waited < 1.0);

    reader.join();
    CHECK(mounted->connection_error().find("smb0") != std::string::npos);
    if (accepted >= 0) ::close(accepted);
    ::close(listener);
}
