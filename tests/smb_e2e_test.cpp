// End-to-end SMB with a login and a password, against a server the harness
// starts itself: mount through the app's own location path, read every
// generated photo byte for byte, run two real conversions from the share and
// save both back into library/ on the same connection, refuse three logins, and
// keep the local copy when the server refuses a write.
//
// Tagged [smb-live] like the other live cases; every case skips itself (with a
// MESSAGE) when the harness environment is absent. Ported from
// runs/v0.2-library/smb-e2e/port/smb_e2e_test.cpp (2026-11-05); the one-mount-
// per-conversion workaround and the diagnostic publish branch were needed only
// while one server error reply killed the connection, and the folder-exists
// restriction only while make_dirs() depended on a failing mkdir. Both are
// fixed (core/vfs/smb.cpp), so this test now asserts the intended behaviour:
//
//   * two conversions in the same run, on one adapter, with library/ absent for
//     the first and present for the second;
//   * a missing file read in between (an error reply, i.e. the case that used
//     to break the connection) and a photo read after it;
//   * three refused logins that say "wrong user name or password".
//
// Environment (see platform/ps5/scripts/live-servers.sh for a server):
//   SPLAT_SMB_URL          smb://<host>:1445/<SHARE>?user=..&password=..
//   SPLAT_SMB_E2E_DIR      local root for state/ (the saved location)
//   SPLAT_SMB_E2E_SHARE    local mirror of the share root (photos/, library/)
//   SPLAT_SMB_E2E_STAGING  local staging root for the upload-then-delete job
//   SPLAT_SMB_E2E_MODEL    the backend's GGUF for a real conversion
//   SPLAT_SMB_E2E_BACKEND  "sharp" (default) or "da2" (the 50 MB fallback
//                          model: the same real reconstruction, much lighter)
//   SPLAT_SMB_E2E_REAL     "1" (default) real; "0" the test double
//   SPLAT_SMB_E2E_ENCRYPT  "1" = the location's SMB3 encryption (seal), to prove
//                          the setting against the server under test
//   SPLAT_SMB_E2E_READONLY share sub-path that exists but refuses writes
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <system_error>

#include "common/path.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "recon/fallback/fallback_reconstructor.h"
#include "recon/reconstructor.h"
#include "recon/sharp/sharp_reconstructor.h"
#include "scene/scene_folder.h"
#include "test_util.h"
#include "vfs/io.h"
#include "vfs/location.h"
#include "vfs/smb.h"

namespace vfs = splat::vfs;
using splat::Path;

namespace {

std::string env_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool e2e_enabled() {
    return !env_string("SPLAT_SMB_URL").empty() && !env_string("SPLAT_SMB_E2E_SHARE").empty();
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
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
    if (at != std::string::npos) rest.erase(0, at + 1);
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
        const std::string pair =
            query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string::npos) out.query[pair.substr(0, equals)] = pair.substr(equals + 1);
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

vfs::Location location_from_env() {
    const ParsedUrl parsed = parse_url(env_string("SPLAT_SMB_URL"));
    vfs::Location live;
    live.id = "smb0";
    live.protocol = vfs::LocationProtocol::smb;
    live.host = parsed.host;
    live.port = parsed.port;
    live.share = parsed.share;
    live.path = parsed.path;
    live.user = parsed.query.count("user") != 0 ? parsed.query.at("user") : std::string();
    live.password =
        parsed.query.count("password") != 0 ? parsed.query.at("password") : std::string();
    live.encrypt = env_string("SPLAT_SMB_E2E_ENCRYPT") == "1";
    return live;
}

Path state_file_path() {
    return Path(env_string("SPLAT_SMB_E2E_DIR")) / "state" / "locations.json";
}

// A fresh store holding exactly one location, saved by the app's own code, so
// the id is always smb0 and the saved-password file is the one under test.
splat::Result<vfs::LocationStore> prepare_store(const vfs::Location& live) {
    const Path file = state_file_path();
    std::error_code error;
    std::filesystem::remove(file, error);
    auto opened = vfs::LocationStore::open(file);
    if (!opened.ok()) return splat::Result<vfs::LocationStore>::failure(opened.message());
    vfs::LocationStore store = opened.take();
    const auto added = store.add(live);
    if (!added.ok()) return splat::Result<vfs::LocationStore>::failure(added.message());
    return splat::Result<vfs::LocationStore>::success(std::move(store));
}

// The location registered in the app registry for the test's duration. The
// registry is global, so a failing assertion must still remove it.
struct MountedLocation {
    std::shared_ptr<vfs::IFileSystem> file_system;
    ~MountedLocation() {
        if (file_system) vfs::file_systems().remove(file_system->id());
    }
};

std::vector<uint8_t> read_local(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

// Every file under `root` with its size: a before/after snapshot proves a
// refused mount wrote nothing on the share. Folders this process may not read
// (a share served by another account creates them 0700 for that account) are
// skipped rather than aborting the walk: the test harness is not necessarily
// the share's owner.
std::map<std::string, uint64_t> snapshot(const std::filesystem::path& root) {
    std::map<std::string, uint64_t> out;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         it != end; it.increment(error)) {
        if (error) break;
        if (!it->is_regular_file(error)) continue;
        out[it->path().lexically_relative(root).generic_string()] = it->file_size(error);
    }
    return out;
}

// The runner's own "fake" double (tests/jobs_upload_test.cpp): a deterministic
// scene with no model, used only when SPLAT_SMB_E2E_REAL=0, and labelled as
// such in the output.
class FakeReconstructor : public splat::IReconstructor {
public:
    splat::Status load(const Path&) override { return splat::Status::success(); }
    void unload() override {}
    splat::BackendInfo info() const override {
        splat::BackendInfo out;
        out.name = "fake";
        return out;
    }
    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput&,
                                         splat::Progress&) override {
        return splat::Result<splat::SplatScene>::success(splat_test::make_test_scene(4));
    }
};

bool real_conversion() { return env_string("SPLAT_SMB_E2E_REAL") != "0"; }

// The backend under test. Both are real reconstructions; da2 is the console's
// fallback and weighs ~50 MB against SHARP's 1.3 GB, which is what makes this
// case runnable inside the memory guard on a 16 GB Mac that is also running the
// test server.
std::string backend_name() {
    const std::string configured = env_string("SPLAT_SMB_E2E_BACKEND");
    return configured.empty() ? std::string("sharp") : configured;
}

splat::JobOutcome run_conversion(const std::string& photo, const Path& destination_root,
                                const Path& staging_root) {
    splat_test::TempDir work{"smb-e2e"};
    splat::JobQueue queue;
    auto queue_opened = splat::JobQueue::open(work.path() / "jobs.json");
    if (!queue_opened.ok()) return splat::JobOutcome{};
    queue = queue_opened.take();
    const auto id = queue.enqueue(vfs::join("smb0", "photos/" + photo),
                                  real_conversion() ? backend_name() : "fake", std::string(), 0,
                                  destination_root);
    if (!id.ok()) return splat::JobOutcome{};

    splat::JobRunnerConfig config;
    config.output_root = work.path() / "scenes-local";
    config.staging_root = staging_root;
    config.model_path = env_string("SPLAT_SMB_E2E_MODEL");
    config.thumbnail_max_edge = 512;
    config.jpeg_quality = 90;
    splat::GpuPhaseGuard guard;
    splat::JobRunner runner(
        queue, guard,
        [](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
            if (backend == "sharp") return std::make_unique<splat::SharpReconstructor>();
            if (backend == "da2") return std::make_unique<splat::FallbackReconstructor>();
            if (backend == "fake") return std::make_unique<FakeReconstructor>();
            return nullptr;
        },
        config);
    const auto outcome = runner.run_next();
    if (!outcome.ok() || !outcome.value().has_value()) return splat::JobOutcome{};
    splat::JobOutcome result = *outcome.value();
    result.job_id = id.value();
    return result;
}

// The four files a published scene is made of, non-empty. The proof is the
// share's own answer - stat and a read through the location path, which is what
// a client sees - and, when this process may look at the share's local mirror,
// the same files on disk. macOS File Sharing creates folders 0700 for the
// account that serves them, so the harness (a different account) often cannot
// stat the mirror; that is a harness limit, not a product one, and it is said
// out loud instead of skipped silently.
void check_published(const std::string& remote_folder, const std::filesystem::path& folder) {
    for (const char* name : {"original.jpg", "thumb.jpg", "scene.psplat", "meta.json"}) {
        const std::string file_name(name);
        const splat::Path remote = vfs::join("smb0", remote_folder + "/" + file_name);
        vfs::FileStat status;
        const splat::Status checked = vfs::stat(remote, &status);
        CHECK_MESSAGE(checked.ok(), file_name << ": " << checked.message());
        CHECK_MESSAGE(status.exists, file_name << " does not exist on the share");
        CHECK_MESSAGE(status.size > 0, file_name << " is empty on the share");
        const splat::Result<std::vector<uint8_t>> bytes = vfs::read_file(remote);
        CHECK_MESSAGE(bytes.ok(), file_name << ": " << bytes.message());
        if (bytes.ok()) CHECK_MESSAGE(!bytes.value().empty(), file_name << " reads back empty");

        std::error_code error;
        const std::uintmax_t on_disk = std::filesystem::file_size(folder / file_name, error);
        if (error) {
            MESSAGE("the local mirror of " << file_name << " is not readable by this process ("
                    << error.message() << "); the share-side read above is the proof");
        } else {
            CHECK(on_disk > 0);
        }
    }
}

}  // namespace

TEST_CASE("the SMB share mounts with a login, lists and reads every photo byte for byte "
          "[smb-live][smb-e2e]") {
    if (!e2e_enabled()) {
        MESSAGE("SPLAT_SMB_URL/SPLAT_SMB_E2E_SHARE are not set: the SMB end-to-end check is skipped");
        return;
    }
    const vfs::Location live = location_from_env();
    const std::string share = env_string("SPLAT_SMB_E2E_SHARE");
    const auto start = std::chrono::steady_clock::now();

    auto prepared = prepare_store(live);
    REQUIRE_MESSAGE(prepared.ok(), prepared.message());
    vfs::LocationStore store = prepared.take();

    // Where a saved password lives, in what form, with what file mode.
    const std::string saved_json = state_file_path().string();
    struct stat saved {};
    REQUIRE(::stat(saved_json.c_str(), &saved) == 0);
    const unsigned mode = static_cast<unsigned>(saved.st_mode) & 0777u;
    char mode_text[8] = {};
    std::snprintf(mode_text, sizeof(mode_text), "%04o", mode);
    MESSAGE("saved location: " << saved_json << " mode " << std::string(mode_text));
    CHECK(mode == 0600u);

    MountedLocation mounted;
    const std::vector<std::string> lines = store.mount_all(vfs::file_systems());
    REQUIRE(lines.size() == 1u);
    MESSAGE("mount: " << lines.front());
    CHECK_MESSAGE(lines.front().find("ready") != std::string::npos, lines.front());
    mounted.file_system = vfs::file_systems().find("smb0");
    REQUIRE(mounted.file_system != nullptr);

    std::error_code error;
    const std::filesystem::path local_photos = std::filesystem::path(share) / "photos";
    std::set<std::string> expected;
    for (const auto& entry : std::filesystem::directory_iterator(local_photos, error)) {
        if (entry.is_regular_file()) expected.insert(entry.path().filename().string());
    }
    REQUIRE_FALSE(expected.empty());

    std::vector<vfs::DirEntry> listed;
    const splat::Status listing = vfs::list(vfs::join("smb0", "photos"), &listed);
    REQUIRE_MESSAGE(listing.ok(), listing.message());
    std::set<std::string> seen;
    for (const vfs::DirEntry& entry : listed) {
        if (!entry.directory) seen.insert(entry.name);
    }
    CHECK(seen == expected);
    MESSAGE("photos/: " << seen.size() << " files listed through the share");
    // macOS File Sharing returns composed (NFC) names; these two are the ones a
    // Polish user's own drive carries, and a folder with a space in its name is
    // the normal shape of a photo album. Both must open by the bytes the
    // listing returned (mac-share PROBE.md, item 2).
    const std::string polish = "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85.jpg";
    for (const std::string nested : {polish, std::string("Rodzina A/zdj\xC4\x99" "cie 1.jpg")}) {
        if (!std::filesystem::exists(local_photos / nested)) continue;
        const auto read = vfs::read_file(vfs::join("smb0", "photos/" + nested));
        REQUIRE_MESSAGE(read.ok(), nested << ": " << read.message());
        const std::vector<uint8_t> local = read_local(local_photos / nested);
        CHECK(read.value() == local);
        MESSAGE("read \"" << nested << "\" (" << local.size() << " bytes) byte for byte");
    }

    // Every file, byte for byte, through the same reader the job runner uses.
    std::uint64_t total = 0;
    for (const std::string& name : expected) {
        const auto read = vfs::read_file(vfs::join("smb0", "photos/" + name));
        REQUIRE_MESSAGE(read.ok(), name << ": " << read.message());
        const std::vector<uint8_t> local = read_local(local_photos / name);
        CHECK(read.value().size() == local.size());
        CHECK(read.value() == local);
        total += local.size();
    }
    MESSAGE("read " << total << " bytes back, all identical; " << seconds_since(start)
                    << " s including connect + mount + save");
}

TEST_CASE("two conversions in one run save into library/ and load back through the location "
          "[smb-live][smb-e2e]") {
    if (!e2e_enabled()) {
        MESSAGE("SPLAT_SMB_URL/SPLAT_SMB_E2E_SHARE are not set: the SMB end-to-end check is skipped");
        return;
    }
    const vfs::Location live = location_from_env();
    const std::string share = env_string("SPLAT_SMB_E2E_SHARE");
    const Path staging_root = env_string("SPLAT_SMB_E2E_STAGING");
    REQUIRE_FALSE(staging_root.string().empty());
    std::error_code error;
    std::filesystem::create_directories(staging_root.string(), error);

    auto prepared = prepare_store(live);
    REQUIRE_MESSAGE(prepared.ok(), prepared.message());
    vfs::LocationStore store = prepared.take();
    MountedLocation mounted;
    const std::vector<std::string> lines = store.mount_all(vfs::file_systems());
    REQUIRE(lines.size() == 1u);
    MESSAGE("mount: " << lines.front());
    REQUIRE_MESSAGE(lines.front().find("ready") != std::string::npos, lines.front());
    mounted.file_system = vfs::file_systems().find("smb0");
    REQUIRE(mounted.file_system != nullptr);

    // Start from the natural first-run state: no library/ on the share. Cleared
    // through the share (the product's own remove_tree), not through the local
    // mirror: when the share is served by another account, macOS creates the
    // folder 0700 for that account and this process cannot touch it on disk.
    // The first job then creates it and the second finds it there, which is the
    // folder that already exists (smb-e2e, BUG 2: make_dirs must not need a
    // failed mkdir).
    (void)vfs::remove_tree(vfs::join("smb0", "library"));
    {
        vfs::FileStat cleared;
        const splat::Status checked = vfs::stat(vfs::join("smb0", "library"), &cleared);
        CHECK_MESSAGE(checked.ok(), checked.message());
        CHECK_FALSE(cleared.exists);
    }

    MESSAGE("reconstruction backend: "
            << (real_conversion() ? backend_name() + " (real conversion)"
                                  : std::string("test double (NOT a real conversion)")));

    struct Case {
        const char* photo;
        const char* folder;
        uint32_t width;
        uint32_t height;
    };
    // The portrait one is stored 1200x800 with EXIF Orientation=6, so the job's
    // meta must say 800x1200: the EXIF turn really went through the pipeline.
    const Case cases[] = {
        {"pattern_1280x720.jpg", "pattern_1280x720", 1280, 720},
        {"portrait_exif_1200x800.jpg", "portrait_exif_1200x800", 800, 1200},
    };

    for (int index = 0; index < 2; ++index) {
        const Case& item = cases[index];
        if (index == 1) {
            // Between the two jobs: a read of a name that is not there. The
            // server answers with an error, which is the reply that used to
            // leave the location broken until the app restarted (BUG 1). The
            // photo read after it must still work.
            const auto missing = vfs::read_file(vfs::join("smb0", "photos/not-here.jpg"));
            CHECK_FALSE(missing.ok());
            MESSAGE("missing file: " << missing.message());
            const auto still_there =
                vfs::read_file(vfs::join("smb0", "photos/" + std::string(item.photo)));
            REQUIRE_MESSAGE(still_there.ok(),
                            "the location broke after one error reply: "
                                << still_there.message());
        }

        const auto start = std::chrono::steady_clock::now();
        const Path destination_root = vfs::join("smb0", "library");
        const splat::JobOutcome outcome = run_conversion(item.photo, destination_root, staging_root);
        MESSAGE("job " << outcome.job_id << " state "
                       << std::string(splat::job_state_name(outcome.state)) << " in "
                       << outcome.seconds_taken << " s: " << outcome.message);
        CHECK_MESSAGE(outcome.state == splat::JobState::Done, outcome.message);

        if (outcome.state != splat::JobState::Done) {
            // The reconstruction still has to be real: the kept staging folder
            // holds the scene, and the message names it.
            const Path kept = staging_root / outcome.job_id;
            const auto kept_meta = splat::read_scene_meta(splat::scene_files(kept).meta);
            CHECK_MESSAGE(kept_meta.ok(), kept_meta.message());
            if (kept_meta.ok() && real_conversion()) {
                // The scene really came from the backend, not the double.
                CHECK(kept_meta.value().backend != "fake");
                CHECK(kept_meta.value().splat_count > 0u);
            }
            continue;
        }

        CHECK(outcome.scene_folder ==
              vfs::join("smb0", std::string("library/") + item.folder));
        check_published(std::string("library/") + item.folder,
                        std::filesystem::path(share) / "library" / item.folder);
        CHECK_FALSE(std::filesystem::exists(staging_root.string() + "/" + outcome.job_id));
        const auto loaded = splat::read_scene_folder(outcome.scene_folder);
        REQUIRE_MESSAGE(loaded.ok(), loaded.message());
        CHECK(loaded.value().meta.intrinsics.width == item.width);
        CHECK(loaded.value().meta.intrinsics.height == item.height);
        CHECK(loaded.value().scene.count() > 0u);
        if (real_conversion()) CHECK(loaded.value().meta.backend != "fake");
        MESSAGE("converted " << item.photo << " -> " << outcome.scene_folder.string() << " ("
                             << loaded.value().scene.count() << " splats, "
                             << seconds_since(start) << " s)");
    }

    // library/ holds both scenes (asked through the share, so it works whoever
    // owns the folder on disk).
    for (const Case& item : cases) {
        vfs::FileStat status;
        const splat::Status checked =
            vfs::stat(vfs::join("smb0", std::string("library/") + item.folder + "/scene.psplat"),
                      &status);
        CHECK_MESSAGE(checked.ok(), item.folder << ": " << checked.message());
        CHECK_MESSAGE(status.exists, item.folder << ": scene.psplat is not on the share");
    }
}

TEST_CASE("wrong password, wrong user and no login say so in plain words "
          "[smb-live][smb-e2e]") {
    if (!e2e_enabled()) {
        MESSAGE("SPLAT_SMB_URL/SPLAT_SMB_E2E_SHARE are not set: the SMB end-to-end check is skipped");
        return;
    }
    const vfs::Location live = location_from_env();
    const std::string share = env_string("SPLAT_SMB_E2E_SHARE");
    const std::map<std::string, uint64_t> before = snapshot(share);
    REQUIRE_FALSE(before.empty());

    struct Case {
        std::string label;
        vfs::Location location;
    };
    std::vector<Case> cases;
    {
        vfs::Location wrong_password = live;
        wrong_password.password = live.password + "-wrong";
        cases.push_back({"wrong password", wrong_password});
        vfs::Location wrong_user = live;
        wrong_user.user = "not-splat-test";
        cases.push_back({"wrong user", wrong_user});
        vfs::Location guest = live;
        guest.user.clear();
        guest.password.clear();
        cases.push_back({"no login", guest});
    }

    for (const Case& item : cases) {
        const auto start = std::chrono::steady_clock::now();
        MountedLocation mounted;
        {
            std::string factory_error;
            mounted.file_system = vfs::make_smb_file_system(item.location, &factory_error);
            REQUIRE_MESSAGE(mounted.file_system != nullptr, factory_error);
            vfs::file_systems().add(mounted.file_system);
        }
        std::vector<vfs::DirEntry> entries;
        const splat::Status listed = vfs::list(vfs::join("smb0", "photos"), &entries);
        const double seconds = seconds_since(start);
        CHECK_MESSAGE(!listed.ok(), item.label);
        CHECK_MESSAGE(listed.message().find("smb0") != std::string::npos, listed.message());
        CHECK_MESSAGE(listed.message().find(live.password) == std::string::npos,
                      "the message must not carry the password");
        // BUG 3: the refusal is the server's answer about the credentials, in
        // words a user can act on, not libsmb2's protocol complaint.
        CHECK_MESSAGE(listed.message().find("wrong user name or password") != std::string::npos,
                      listed.message());
        CHECK_MESSAGE(entries.empty(), item.label);
        MESSAGE("refused (" << item.label << ", " << seconds << " s): " << listed.message());
    }

    // Nothing was written: the share's file set is identical afterwards.
    const std::map<std::string, uint64_t> after = snapshot(share);
    CHECK(after == before);
}

TEST_CASE("a write refused by the server keeps the local copy and the job says where "
          "[smb-live][smb-e2e]") {
    if (!e2e_enabled()) {
        MESSAGE("SPLAT_SMB_URL/SPLAT_SMB_E2E_SHARE are not set: the SMB end-to-end check is skipped");
        return;
    }
    const vfs::Location live = location_from_env();
    const std::string share = env_string("SPLAT_SMB_E2E_SHARE");
    const std::string read_only = env_string("SPLAT_SMB_E2E_READONLY");
    if (read_only.empty()) {
        MESSAGE("SPLAT_SMB_E2E_READONLY is not set: the refused-upload check is skipped");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const Path staging_root = env_string("SPLAT_SMB_E2E_STAGING");
    std::error_code error;
    std::filesystem::create_directories(staging_root.string(), error);

    auto prepared = prepare_store(live);
    REQUIRE_MESSAGE(prepared.ok(), prepared.message());
    vfs::LocationStore store = prepared.take();
    MountedLocation mounted;
    const std::vector<std::string> lines = store.mount_all(vfs::file_systems());
    REQUIRE_FALSE(lines.empty());
    mounted.file_system = vfs::file_systems().find("smb0");
    REQUIRE(mounted.file_system != nullptr);

    // A tiny .splat (two splats) so the job reaches the upload without a model.
    splat_test::TempDir work{"smb-e2e-ro"};
    const Path splat_file = work.path() / "tiny.splat";
    {
        std::vector<uint8_t> tiny(64, 0);
        for (int splat = 0; splat < 2; ++splat) {
            const std::size_t base = static_cast<std::size_t>(splat) * 32;
            tiny[base + 24 + 0] = 255;
            tiny[base + 24 + 3] = 255;
            tiny[base + 28 + 3] = 255;
        }
        REQUIRE_MESSAGE(vfs::write_file(splat_file, tiny).ok(), "cannot write the tiny splat");
    }

    auto queue_opened = splat::JobQueue::open(work.path() / "jobs.json");
    REQUIRE_MESSAGE(queue_opened.ok(), queue_opened.message());
    splat::JobQueue queue = queue_opened.take();
    const auto id =
        queue.enqueue_import(splat_file, std::string(), vfs::join("smb0", read_only + "/Album"));
    REQUIRE_MESSAGE(id.ok(), id.message());

    splat::GpuPhaseGuard guard;
    splat::JobRunnerConfig config;
    config.output_root = work.path() / "scenes-local";
    config.staging_root = staging_root;
    config.model_path = work.path() / "model.gguf";
    splat::JobRunner runner(queue, guard, nullptr, config);
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK_MESSAGE(outcome.value()->state == splat::JobState::Failed,
                  "the server should have refused the write");
    const std::string message = outcome.value()->message;
    MESSAGE("refused upload (" << seconds_since(start) << " s): " << message);
    CHECK(message.find("upload failed:") != std::string::npos);
    CHECK(message.find("kept at") != std::string::npos);
    CHECK(message.find(live.password) == std::string::npos);
    const std::string kept = staging_root.string() + "/" + id.value();
    CHECK(std::filesystem::exists(kept + "/scene.psplat"));
    CHECK(std::filesystem::exists(kept + "/meta.json"));
    // And the share really is untouched.
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(share) / read_only / "Album"));
}
