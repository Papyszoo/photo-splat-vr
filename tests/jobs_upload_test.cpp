// Upload-then-delete (L3-C): a conversion whose album is on a network location
// is built in <staging_root>/<job id>, copied to the share, verified and then
// removed; a failed upload keeps the staging folder and names it in the job's
// message. The destination is an in-memory fake registered as "fake".
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "gallery/gallery.h"
#include "gallery/photo_sources.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "jobs/staging.h"
#include "recon/reconstructor.h"
#include "scene/ply.h"
#include "test_util.h"
#include "ui/job_service.h"
#include "ui/services.h"
#include "ui/ui_state.h"
#include "vfs/io.h"

using splat::Path;

namespace {

// The destination share, in memory. `fail_writes` makes every write fail with
// the fake's own message while the share still answers (a refused write);
// `offline` makes every call fail, which is a share that is switched off.
class FakeShare : public splat::vfs::IFileSystem {
public:
    std::string id() const override { return "fake"; }
    std::string describe() const override { return "fake:/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t) override {
        if (offline) return splat::Result<std::vector<uint8_t>>::failure("fake: offline");
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        if (offline) return splat::Status::error("fake: offline");
        if (on_write) on_write();
        if (fail_writes) return splat::Status::error("fake share refused the write");
        files[relative] = std::vector<uint8_t>(data, data + size);
        return splat::Status::success();
    }
    splat::Status list(const std::string& relative, std::vector<splat::vfs::DirEntry>* entries) override {
        if (offline) return splat::Status::error("fake: offline");
        for (const std::string& directory : directories) {
            if (leaf(directory) == "." || !child_of(relative, directory)) continue;
            splat::vfs::DirEntry entry;
            entry.name = leaf(directory);
            entry.directory = true;
            entries->push_back(entry);
        }
        for (const auto& file : files) {
            if (!child_of(relative, file.first)) continue;
            splat::vfs::DirEntry entry;
            entry.name = leaf(file.first);
            entry.size = file.second.size();
            entries->push_back(entry);
        }
        return splat::Status::success();
    }
    splat::Status stat(const std::string& relative, splat::vfs::FileStat* out) override {
        *out = splat::vfs::FileStat{};
        if (offline) return splat::Status::error("fake: offline");
        const auto found = files.find(relative);
        if (found != files.end()) {
            out->exists = true;
            out->regular = true;
            out->size = found->second.size();
            return splat::Status::success();
        }
        if (directories.count(relative) != 0 || relative.empty()) {
            out->exists = true;
            out->directory = true;
        }
        return splat::Status::success();
    }
    splat::Status make_dirs(const std::string& relative) override {
        if (offline) return splat::Status::error("fake: offline");
        // Every folder on the way exists too, as on a real share.
        for (std::size_t slash = relative.find('/'); slash != std::string::npos;
             slash = relative.find('/', slash + 1)) {
            directories.insert(relative.substr(0, slash));
        }
        directories.insert(relative);
        return splat::Status::success();
    }
    splat::Status remove_file(const std::string& relative) override {
        if (files.erase(relative) == 0) {
            return splat::Status::error("fake: no such file: " + relative);
        }
        return splat::Status::success();
    }
    splat::Status remove_tree(const std::string& relative) override {
        if (offline) return splat::Status::error("fake: offline");
        for (auto it = files.begin(); it != files.end();) {
            it = under(relative, it->first) ? files.erase(it) : std::next(it);
        }
        for (auto it = directories.begin(); it != directories.end();) {
            it = (*it == relative || under(relative, *it)) ? directories.erase(it)
                                                           : std::next(it);
        }
        return splat::Status::success();
    }
    // Like SMB: a folder moves with everything below it, and a name that is
    // taken is refused (libsmb2 sends ReplaceIfExists=0).
    bool rename_replaces() const override { return false; }
    splat::Status rename(const std::string& from, const std::string& to) override {
        if (offline) return splat::Status::error("fake: offline");
        if (files.count(to) != 0 || directories.count(to) != 0) {
            return splat::Status::error("fake: " + to + " exists");
        }
        if (directories.count(from) == 0 && files.count(from) == 0) {
            return splat::Status::error("fake: no such file: " + from);
        }
        std::map<std::string, std::vector<uint8_t>> moved_files;
        for (auto it = files.begin(); it != files.end();) {
            if (it->first == from || under(from, it->first)) {
                moved_files[to + it->first.substr(from.size())] = std::move(it->second);
                it = files.erase(it);
            } else {
                ++it;
            }
        }
        std::set<std::string> moved_directories;
        for (auto it = directories.begin(); it != directories.end();) {
            if (*it == from || under(from, *it)) {
                moved_directories.insert(to + it->substr(from.size()));
                it = directories.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& file : moved_files) files[file.first] = std::move(file.second);
        directories.insert(moved_directories.begin(), moved_directories.end());
        return splat::Status::success();
    }

    bool contains(const std::string& relative) const { return files.count(relative) != 0; }

    static std::string leaf(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    static bool under(const std::string& parent, const std::string& path) {
        return path.size() > parent.size() + 1 &&
               path.compare(0, parent.size(), parent) == 0 && path[parent.size()] == '/';
    }
    static bool child_of(const std::string& parent, const std::string& path) {
        const std::string prefix = parent.empty() ? std::string() : parent + "/";
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) {
            return false;
        }
        return path.find('/', prefix.size()) == std::string::npos;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> directories;
    bool fail_writes = false;
    std::atomic<bool> offline{false};
    std::function<void()> on_write;  // runs inside every write, to act "meanwhile"
};

class FakeReconstructor : public splat::IReconstructor {
public:
    splat::Status load(const Path&) override { return splat::Status::success(); }
    void unload() override {}
    splat::BackendInfo info() const override {
        splat::BackendInfo out;
        out.name = "fake";
        return out;
    }
    splat::Result<splat::SplatScene> run(const splat::ReconstructionInput& input,
                                         splat::Progress&) override {
        (void)input;
        return splat::Result<splat::SplatScene>::success(splat_test::make_test_scene(4));
    }
};

struct Fixture {
    splat_test::TempDir dir{"jobs-upload"};
    std::shared_ptr<FakeShare> share = std::make_shared<FakeShare>();
    Path scenes_root = dir.path() / "scenes";
    Path staging_root = dir.path() / "staging";
    Path photo = dir.path() / "photo.jpg";
    splat::GpuPhaseGuard guard;
    splat::JobQueue queue;

    Fixture() {
        splat::vfs::file_systems().add(share);
        std::error_code error;
        std::filesystem::create_directories(staging_root, error);

        splat::Image image;
        image.resize(32, 24);
        for (int y = 0; y < 24; ++y) {
            for (int x = 0; x < 32; ++x) {
                uint8_t* pixel = image.pixel(x, y);
                pixel[0] = static_cast<uint8_t>(x * 4);
                pixel[1] = static_cast<uint8_t>(y * 4);
                pixel[2] = 100;
            }
        }
        const auto jpeg = splat::encode_jpeg(image, 85);
        REQUIRE_MESSAGE(jpeg.ok(), jpeg.message());
        std::ofstream out(photo, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(jpeg.value().data()),
                  static_cast<std::streamsize>(jpeg.value().size()));
        REQUIRE(static_cast<bool>(out));

        const auto opened = splat::JobQueue::open(dir.path() / "jobs.json");
        REQUIRE_MESSAGE(opened.ok(), opened.message());
        queue = opened.value();
    }

    ~Fixture() { splat::vfs::file_systems().remove("fake"); }

    // `retry_seconds` is the first wait before an upload is tried again; 0 makes
    // every Waiting job due at once, so a test steps it with run_next().
    splat::JobRunner runner(double retry_seconds = 0.0) {
        splat::JobRunnerConfig config;
        config.output_root = scenes_root;
        config.staging_root = staging_root;
        config.model_path = dir.path() / "model.gguf";
        config.thumbnail_max_edge = 128;
        config.retry_initial_seconds = retry_seconds;
        config.retry_max_seconds = retry_seconds * 4.0;
        config.location_name = [](const std::string& id) {
            return id == "fake" ? std::string("Test NAS") : std::string();
        };
        return splat::JobRunner(
            queue, guard,
            [](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
                if (backend != "fake") return nullptr;
                return std::make_unique<FakeReconstructor>();
            },
            config);
    }
};

std::string job_message(const splat::JobQueue& queue, const std::string& id) {
    const splat::Job* job = queue.find(id);
    return job == nullptr ? std::string() : job->message;
}

}  // namespace

TEST_CASE("a converted scene for a share is uploaded and its staging copy removed") {
    Fixture fixture;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "Album", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());

    splat::JobRunner runner = fixture.runner();
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Done);

    // The four scene files are on the share, under the album.
    const std::string folder = "scenes/Album/photo";
    for (const char* name : {"original.jpg", "thumb.jpg", "scene.psplat", "meta.json"}) {
        CHECK_MESSAGE(fixture.share->contains(folder + "/" + name), name);
    }
    // The job reports the destination, and the staging copy is gone.
    CHECK(outcome.value()->scene_folder == Path("fake:/" + folder));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
}

TEST_CASE("a conversion and an import land in <output folder>/<album>/<scene>") {
    Fixture fixture;
    const Path output("fake:/scenes/Made");
    const Path cloud = fixture.dir.path() / "cloud.ply";
    REQUIRE(splat::export_ply(cloud, splat_test::make_test_scene(4)).ok());
    splat::JobRunner runner = fixture.runner();

    const auto run_one = [&](const char* what) {
        const auto outcome = runner.run_next();
        REQUIRE_MESSAGE(outcome.ok(), outcome.message());
        REQUIRE_MESSAGE(outcome.value().has_value(), what);
        CHECK_MESSAGE(outcome.value()->state == splat::JobState::Done,
                      what << ": " << outcome.value()->message);
        return outcome.value()->scene_folder;
    };

    // A photo converted from the album "Trips", and a splat file imported into it.
    REQUIRE(fixture.queue.enqueue(fixture.photo, "fake", "Trips", 0, output).ok());
    CHECK(run_one("conversion into an album") == Path("fake:/scenes/Made/Trips/photo"));
    REQUIRE(fixture.queue.enqueue_import(cloud, "Trips", output).ok());
    CHECK(run_one("import into an album") == Path("fake:/scenes/Made/Trips/cloud"));

    // No album (the console's Unsorted): the scene is loose in the output folder.
    REQUIRE(fixture.queue.enqueue(fixture.photo, "fake", "", 0, output).ok());
    CHECK(run_one("conversion without an album") == Path("fake:/scenes/Made/photo"));
    REQUIRE(fixture.queue.enqueue_import(cloud, "", output).ok());
    CHECK(run_one("import without an album") == Path("fake:/scenes/Made/cloud"));

    for (const char* scene : {"Trips/photo", "Trips/cloud", "photo", "cloud"}) {
        for (const char* name : {"scene.psplat", "meta.json", "thumb.jpg"}) {
            CHECK_MESSAGE(fixture.share->contains(std::string("scenes/Made/") + scene + "/" + name),
                          scene << "/" << name);
        }
    }
    // Nothing was kept on the console, and no scene went to the local library.
    CHECK_FALSE(std::filesystem::exists(fixture.scenes_root));
}

TEST_CASE("a failed upload keeps the staging folder and says where it is") {
    Fixture fixture;
    fixture.share->fail_writes = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "Album", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());

    splat::JobRunner runner = fixture.runner();
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Failed);

    const std::string message = job_message(fixture.queue, id.value());
    CHECK(message.find("upload failed:") != std::string::npos);
    CHECK(message.find("kept at") != std::string::npos);
    CHECK(message.find("fake share refused the write") != std::string::npos);
    // The staging copy really is still there, with the finished scene.
    CHECK(std::filesystem::exists(fixture.staging_root / id.value() / "scene.psplat"));
    CHECK(std::filesystem::exists(fixture.staging_root / id.value() / "meta.json"));
}

TEST_CASE("boot sweeps the staging folders no live job can need") {
    Fixture fixture;
    const auto make_folder = [&](const std::string& name) {
        const Path folder = fixture.staging_root / name;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        std::ofstream out(folder / "scene.psplat", std::ios::binary | std::ios::trunc);
        out << "not a real scene";
        REQUIRE(static_cast<bool>(out));
    };

    // One job of each terminal state plus a folder that names no job at all:
    // the two a Done/Cancelled job left behind and the orphan go, the Failed,
    // Queued and Running ones stay because they are still the only copy.
    splat::Job failed;
    failed.id = "job-000900";
    failed.state = splat::JobState::Failed;
    splat::Job queued;
    queued.id = "job-000901";
    queued.state = splat::JobState::Queued;
    splat::Job running;
    running.id = "job-000902";
    running.state = splat::JobState::Running;
    splat::Job done;
    done.id = "job-000903";
    done.state = splat::JobState::Done;
    splat::Job cancelled;
    cancelled.id = "job-000904";
    cancelled.state = splat::JobState::Cancelled;
    const std::vector<splat::Job> jobs = {failed, queued, running, done, cancelled};

    for (const splat::Job& job : jobs) make_folder(job.id);
    make_folder("orphan-000905");  // a crash before the queue was saved

    const std::vector<std::string> removed = splat::sweep_staging(fixture.staging_root, jobs);
    CHECK(removed == std::vector<std::string>{"job-000903", "job-000904", "orphan-000905"});
    CHECK(std::filesystem::exists(fixture.staging_root / "job-000900" / "scene.psplat"));
    CHECK(std::filesystem::exists(fixture.staging_root / "job-000901"));
    CHECK(std::filesystem::exists(fixture.staging_root / "job-000902"));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / "job-000903"));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / "job-000904"));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / "orphan-000905"));

    // A missing staging folder is not an error: the first boot has none.
    CHECK(splat::sweep_staging(fixture.dir.path() / "no-such-tmp", jobs).empty());
}

// ---- Waiting for an offline share (0.6.0) ------------------------------------

namespace {

std::filesystem::path staged_scene(const Fixture& fixture, const std::string& id) {
    return fixture.staging_root / id / "scene.psplat";
}

// One configured location, "fake", for the Output folder lookup.
class StubLocations : public splat::ILocationService {
public:
    std::vector<splat::LocationView> list() const override {
        splat::LocationView view;
        view.id = "fake";
        view.name = "Test NAS";
        view.splat_library = true;
        return {view};
    }
    std::string status(const std::string&) const override { return "ready"; }
    splat::Status add(const splat::LocationView&, std::string*) override {
        return splat::Status::error("stub");
    }
    splat::Status update(const splat::LocationView&) override { return splat::Status::error("stub"); }
    splat::Status remove(const std::string&) override { return splat::Status::error("stub"); }
    splat::Status mount(const std::string&) override { return splat::Status::error("stub"); }
    void unmount(const std::string&) override {}
};

}  // namespace

TEST_CASE("a scene for a share that is switched off waits, then is saved when it is back") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());

    splat::JobRunner runner = fixture.runner();
    const auto first = runner.run_next();
    REQUIRE_MESSAGE(first.ok(), first.message());
    REQUIRE(first.value().has_value());
    // Not a failure: the row says what it waits for, and the finished scene is
    // kept in the job's own staging folder (nothing else is kept).
    CHECK(first.value()->state == splat::JobState::Waiting);
    CHECK(job_message(fixture.queue, id.value()) == "Waiting for Test NAS");
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));
    CHECK(std::filesystem::exists(fixture.staging_root / id.value() / "meta.json"));
    CHECK(fixture.share->files.empty());
    CHECK(runner.next_retry_seconds().has_value());

    // The share is still off: another attempt changes nothing.
    const auto second = runner.run_next();
    REQUIRE_MESSAGE(second.ok(), second.message());
    REQUIRE(second.value().has_value());
    CHECK(second.value()->state == splat::JobState::Waiting);
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));

    // Back on: the next attempt uploads the same scene and removes the copy.
    fixture.share->offline = false;
    const auto third = runner.run_next();
    REQUIRE_MESSAGE(third.ok(), third.message());
    REQUIRE(third.value().has_value());
    CHECK(third.value()->state == splat::JobState::Done);
    CHECK(third.value()->scene_folder == Path("fake:/scenes/photo"));
    for (const char* name : {"original.jpg", "thumb.jpg", "scene.psplat", "meta.json"}) {
        CHECK_MESSAGE(fixture.share->contains(std::string("scenes/photo/") + name), name);
    }
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
    CHECK_FALSE(runner.next_retry_seconds().has_value());
    // Nothing is left to run.
    const auto idle = runner.run_next();
    REQUIRE(idle.ok());
    CHECK_FALSE(idle.value().has_value());
}

TEST_CASE("a waiting job is not a second copy of its photo, and conversions go on behind it") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto first = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(first.ok(), first.message());
    splat::JobRunner runner = fixture.runner(60.0);  // not due again within the test
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    // The same photo is still in flight: the queue refuses a duplicate.
    const auto again = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    CHECK_FALSE(again.ok());

    // A job for the console is not held up by the waiting one.
    const Path other = fixture.dir.path() / "other.jpg";
    std::filesystem::copy_file(fixture.photo, other);
    const auto local = fixture.queue.enqueue(other, "fake");
    REQUIRE_MESSAGE(local.ok(), local.message());
    const auto ran = runner.run_next();
    REQUIRE_MESSAGE(ran.ok(), ran.message());
    REQUIRE(ran.value().has_value());
    CHECK(ran.value()->job_id == local.value());
    CHECK(ran.value()->state == splat::JobState::Done);
    CHECK(fixture.queue.find(first.value())->state == splat::JobState::Waiting);
}

TEST_CASE("a waiting job survives a restart and is retried at once") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    {
        splat::JobRunner runner = fixture.runner(3600.0);  // an hour: never due in a test
        REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);
        CHECK(runner.next_retry_seconds().value_or(0.0) > 60.0);
        const auto not_yet = runner.run_next();
        REQUIRE(not_yet.ok());
        CHECK_FALSE(not_yet.value().has_value());
    }

    // The process is gone. The queue file still says "waiting"; the finished
    // scene is still on disk and the boot sweep leaves it alone.
    auto reopened = splat::JobQueue::open(fixture.dir.path() / "jobs.json");
    REQUIRE_MESSAGE(reopened.ok(), reopened.message());
    splat::JobQueue queue = reopened.take();
    REQUIRE(queue.find(id.value()) != nullptr);
    CHECK(queue.find(id.value())->state == splat::JobState::Waiting);
    CHECK(queue.find(id.value())->output_root == Path("fake:/scenes"));
    CHECK(queue.find(id.value())->message == "Waiting for Test NAS");
    CHECK(splat::sweep_staging(fixture.staging_root, queue.jobs()).empty());
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));

    // A new process has no schedule: the first run_next retries, and the share
    // being back is enough.
    fixture.share->offline = false;
    splat::JobRunnerConfig config;
    config.output_root = fixture.scenes_root;
    config.staging_root = fixture.staging_root;
    config.retry_initial_seconds = 3600.0;
    splat::JobRunner restarted(queue, fixture.guard,
                               [](const std::string&) -> std::unique_ptr<splat::IReconstructor> {
                                   return nullptr;  // an upload retry never loads a model
                               },
                               config);
    CHECK(restarted.next_retry_seconds().value_or(1.0) == 0.0);
    const auto outcome = restarted.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Done);
    CHECK(fixture.share->contains("scenes/photo/scene.psplat"));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
    // The row is Done in the file too.
    const auto final_queue = splat::JobQueue::open(fixture.dir.path() / "jobs.json");
    REQUIRE(final_queue.ok());
    CHECK(final_queue.value().find(id.value())->state == splat::JobState::Done);
}

TEST_CASE("a name taken on the share while the job waited is not overwritten") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    // Another scene called "photo" appears on the share meanwhile.
    fixture.share->offline = false;
    fixture.share->directories.insert("scenes/photo");
    fixture.share->files["scenes/photo/meta.json"] = {'o', 'l', 'd'};
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Done);
    CHECK(outcome.value()->scene_folder == Path("fake:/scenes/photo-2"));
    CHECK(fixture.share->files["scenes/photo/meta.json"] == std::vector<uint8_t>{'o', 'l', 'd'});
    CHECK(fixture.share->contains("scenes/photo-2/scene.psplat"));
}

TEST_CASE("cancelling a waiting job removes its temporary files") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner(3600.0);
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);
    REQUIRE(std::filesystem::exists(staged_scene(fixture, id.value())));

    // The queue takes the cancel, the runner removes the staging copy.
    REQUIRE(fixture.queue.cancel(id.value()).ok());
    runner.discard_waiting(id.value());
    CHECK(fixture.queue.find(id.value())->state == splat::JobState::Cancelled);
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
    // And a cancelled job is never retried.
    const auto idle = runner.run_next();
    REQUIRE(idle.ok());
    CHECK_FALSE(idle.value().has_value());
    CHECK_FALSE(runner.next_retry_seconds().has_value());
    CHECK(fixture.share->files.empty());
}

// The names under `folder` on the share, files and folders, whatever their depth.
std::vector<std::string> share_entries(const FakeShare& share, const std::string& folder) {
    std::vector<std::string> out;
    const std::string prefix = folder + "/";
    for (const auto& file : share.files) {
        if (file.first.compare(0, prefix.size(), prefix) == 0) out.push_back(file.first);
    }
    for (const std::string& directory : share.directories) {
        if (directory.compare(0, prefix.size(), prefix) == 0) out.push_back(directory);
    }
    return out;
}

// The share drops after `after_writes` files of the upload have arrived.
void drop_after(Fixture& fixture, int after_writes) {
    auto writes = std::make_shared<int>(0);
    fixture.share->on_write = [&fixture, writes, after_writes] {
        if (++*writes == after_writes) fixture.share->offline = true;
    };
}

TEST_CASE("a share that drops mid-upload never shows a partial scene, and the retry saves exactly one") {
    Fixture fixture;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "Album", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();

    // Two files arrive, then the share is gone.
    drop_after(fixture, 2);
    const auto dropped = runner.run_next();
    REQUIRE_MESSAGE(dropped.ok(), dropped.message());
    REQUIRE(dropped.value().has_value());
    CHECK(dropped.value()->state == splat::JobState::Waiting);
    fixture.share->on_write = nullptr;

    // What arrived is in the job's own hidden temporary folder, never under the
    // scene's name (the share was off, so it could not be cleaned up yet).
    const std::string temporary = "scenes/Album/.upload-" + id.value();
    CHECK(share_entries(*fixture.share, "scenes/Album/photo").empty());
    CHECK(fixture.share->directories.count("scenes/Album/photo") == 0);
    CHECK(fixture.share->files.size() == 2);
    for (const auto& file : fixture.share->files) {
        CHECK_MESSAGE(file.first.compare(0, temporary.size() + 1, temporary + "/") == 0, file.first);
    }

    // The share is back before the retry: no listing shows the partial scene.
    fixture.share->offline = false;
    const Path root("fake:/scenes");
    CHECK(splat::list_scenes(root, "Album").empty());
    for (const splat::AlbumEntry& album : splat::list_albums(root)) {
        CHECK_MESSAGE(album.name.rfind(".", 0) != 0, album.name);
        CHECK_MESSAGE(album.scene_count == 0, album.name);
    }
    for (const splat::FolderEntry& entry : splat::list_folder(root / "Album")) {
        CHECK_MESSAGE(entry.name.rfind(".", 0) != 0, entry.name);
    }

    // The retry removes its own leftover first, then saves under the plain name.
    const auto retried = runner.run_next();
    REQUIRE_MESSAGE(retried.ok(), retried.message());
    REQUIRE(retried.value().has_value());
    CHECK(retried.value()->state == splat::JobState::Done);
    CHECK(retried.value()->scene_folder == Path("fake:/scenes/Album/photo"));
    for (const char* name : {"original.jpg", "thumb.jpg", "scene.psplat", "meta.json"}) {
        CHECK_MESSAGE(fixture.share->contains(std::string("scenes/Album/photo/") + name), name);
    }
    // Exactly one scene folder, nothing else: no "-2", no temporary folder.
    CHECK(fixture.share->files.size() == 4);
    const std::string scene_prefix = "scenes/Album/photo/";
    for (const auto& file : fixture.share->files) {
        CHECK_MESSAGE(file.first.compare(0, scene_prefix.size(), scene_prefix) == 0, file.first);
    }
    for (const std::string& directory : fixture.share->directories) {
        CHECK_MESSAGE(directory.find(".upload-") == std::string::npos, directory);
        CHECK_MESSAGE(directory.find("photo-2") == std::string::npos, directory);
    }
    CHECK(splat::list_scenes(root, "Album").size() == 1);
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
}

TEST_CASE("a share that answers and then refuses mid-upload leaves nothing on it") {
    Fixture fixture;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "Album", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    auto writes = std::make_shared<int>(0);
    fixture.share->on_write = [&fixture, writes] {
        if (++*writes == 2) fixture.share->fail_writes = true;
    };
    splat::JobRunner runner = fixture.runner();
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Failed);
    // The share is up, so the half-copied temporary folder was removed at once.
    CHECK(fixture.share->files.empty());
    for (const std::string& directory : fixture.share->directories) {
        CHECK_MESSAGE(directory.find(".upload-") == std::string::npos, directory);
    }
    // The finished scene is still on the console.
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));
}

TEST_CASE("cancelling a waiting job whose upload dropped removes the partial copy") {
    Fixture fixture;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "Album", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner(3600.0);
    drop_after(fixture, 2);
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);
    fixture.share->on_write = nullptr;
    CHECK(fixture.share->files.size() == 2);

    // The user cancels (the share is still off, so nothing can be removed from it
    // yet) and the share comes back: the runner removes the partial copy the next
    // time it runs, whatever the queue holds.
    REQUIRE(fixture.queue.cancel(id.value()).ok());
    runner.discard_waiting(id.value());
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
    fixture.share->offline = false;
    const auto idle = runner.run_next();
    REQUIRE(idle.ok());
    CHECK_FALSE(idle.value().has_value());
    CHECK(fixture.share->files.empty());
    for (const std::string& directory : fixture.share->directories) {
        CHECK_MESSAGE(directory.find(".upload-") == std::string::npos, directory);
    }
}

TEST_CASE("a job cancelled while its retry uploads leaves nothing on the share or the console") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    // The share is back, and the user cancels as the first file arrives.
    fixture.share->offline = false;
    fixture.share->on_write = [&] {
        fixture.share->on_write = nullptr;
        REQUIRE(fixture.queue.cancel(id.value()).ok());
    };
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Cancelled);
    CHECK(fixture.queue.find(id.value())->state == splat::JobState::Cancelled);
    CHECK(fixture.share->files.empty());
    for (const std::string& directory : fixture.share->directories) {
        CHECK_MESSAGE(directory.find(".upload-") == std::string::npos, directory);
    }
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / id.value()));
}

TEST_CASE("a waiting job whose finished scene is gone fails with a message") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    std::filesystem::remove_all(fixture.staging_root / id.value());
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Failed);
    CHECK(job_message(fixture.queue, id.value()).find("no longer on this console") !=
          std::string::npos);
    CHECK_FALSE(runner.next_retry_seconds().has_value());
}

TEST_CASE("a share that answers and then refuses the retry fails the waiting job") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    fixture.share->offline = false;
    fixture.share->fail_writes = true;  // up, but it will not take a file
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Failed);
    const std::string message = job_message(fixture.queue, id.value());
    CHECK(message.find("upload failed:") != std::string::npos);
    CHECK(message.find("kept at") != std::string::npos);
    // The failed row keeps the finished scene, as every failed upload does.
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));
}

TEST_CASE("a location being re-mounted after an edit keeps its job waiting") {
    Fixture fixture;
    fixture.share->offline = true;
    const auto id = fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("fake:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    REQUIRE(runner.run_next().value()->state == splat::JobState::Waiting);

    // Editing the location drops its adapter until the new mount answers. The
    // store still knows the location ("Test NAS"), so the job keeps waiting
    // instead of failing as if the location had been removed.
    splat::vfs::file_systems().remove("fake");
    const auto during = runner.run_next();
    REQUIRE_MESSAGE(during.ok(), during.message());
    REQUIRE(during.value().has_value());
    CHECK(during.value()->state == splat::JobState::Waiting);
    CHECK(std::filesystem::exists(staged_scene(fixture, id.value())));

    splat::vfs::file_systems().add(fixture.share);
    fixture.share->offline = false;
    const auto after = runner.run_next();
    REQUIRE_MESSAGE(after.ok(), after.message());
    REQUIRE(after.value().has_value());
    CHECK(after.value()->state == splat::JobState::Done);
    CHECK(fixture.share->contains("scenes/photo/scene.psplat"));
}

TEST_CASE("a location this console does not know is a failure, not a wait") {
    Fixture fixture;
    const auto id =
        fixture.queue.enqueue(fixture.photo, "fake", "", 0, Path("nowhere:/scenes"));
    REQUIRE_MESSAGE(id.ok(), id.message());
    splat::JobRunner runner = fixture.runner();
    const auto outcome = runner.run_next();
    REQUIRE_MESSAGE(outcome.ok(), outcome.message());
    REQUIRE(outcome.value().has_value());
    CHECK(outcome.value()->state == splat::JobState::Failed);
    CHECK(job_message(fixture.queue, id.value()).find("upload failed:") != std::string::npos);
}

TEST_CASE("the upload retry delay doubles up to its cap") {
    using splat::upload_retry_delay_seconds;
    CHECK(upload_retry_delay_seconds(1, 5.0, 120.0) == doctest::Approx(5.0));
    CHECK(upload_retry_delay_seconds(2, 5.0, 120.0) == doctest::Approx(10.0));
    CHECK(upload_retry_delay_seconds(3, 5.0, 120.0) == doctest::Approx(20.0));
    CHECK(upload_retry_delay_seconds(5, 5.0, 120.0) == doctest::Approx(80.0));
    CHECK(upload_retry_delay_seconds(6, 5.0, 120.0) == doctest::Approx(120.0));
    CHECK(upload_retry_delay_seconds(60, 5.0, 120.0) == doctest::Approx(120.0));
    CHECK(upload_retry_delay_seconds(0, 5.0, 120.0) == doctest::Approx(5.0));
    CHECK(upload_retry_delay_seconds(3, 0.0, 0.0) == doctest::Approx(0.0));
    CHECK(upload_retry_delay_seconds(3, -4.0, 10.0) == doctest::Approx(0.0));
}

TEST_CASE("the job service retries a waiting job when its share comes back, and cancel cleans up") {
    Fixture fixture;
    fixture.share->offline = true;
    splat::JobRunnerConfig config;
    config.output_root = fixture.scenes_root;
    config.staging_root = fixture.staging_root;
    config.model_path = fixture.dir.path() / "model.gguf";
    config.thumbnail_max_edge = 128;
    config.retry_initial_seconds = 0.05;
    config.retry_max_seconds = 0.2;
    config.location_name = [](const std::string&) { return std::string("Test NAS"); };
    splat::JobRunner runner(
        fixture.queue, fixture.guard,
        [](const std::string& backend) -> std::unique_ptr<splat::IReconstructor> {
            if (backend != "fake") return nullptr;
            return std::make_unique<FakeReconstructor>();
        },
        config);
    splat::JobService service(fixture.queue, runner);

    // The setting decides the destination: the chosen share folder, and the
    // shelf's album under it.
    splat::UiState state;
    state.output_folder = "fake:/scenes/Made";
    REQUIRE(splat::valid_output_folder(state.output_folder));
    StubLocations locations;
    // A conversion started from the console's own shelf "Trips" goes to the
    // chosen folder, into the album "Trips"; with no such location it is refused
    // rather than saved on the console.
    const auto target = splat::scene_target(&state, &locations, fixture.scenes_root, "Trips");
    REQUIRE_MESSAGE(target.ok(), target.message());
    CHECK(target.value().root == Path("fake:/scenes/Made"));
    CHECK(target.value().album == "Trips");
    CHECK(target.value().from_setting);
    CHECK_FALSE(splat::scene_target(&state, nullptr, fixture.scenes_root, "Trips").ok());
    const splat::SceneTarget chosen = target.value();

    REQUIRE(service.enqueue({fixture.photo}, "fake", chosen.album, chosen.root).ok());
    REQUIRE(splat_test::wait_until([&] {
        const std::vector<splat::JobView> views = service.snapshot();
        return views.size() == 1 && views[0].state == "waiting";
    }));
    const std::vector<splat::JobView> waiting = service.snapshot();
    CHECK(waiting[0].message == "Waiting for Test NAS");
    CHECK_FALSE(service.busy());

    // The share comes back: the worker's own alarm retries, with no input.
    fixture.share->offline = false;
    REQUIRE(splat_test::wait_until([&] {
        const std::vector<splat::JobView> views = service.snapshot();
        return views.size() == 1 && views[0].state == "done";
    }));
    CHECK(fixture.share->contains("scenes/Made/Trips/photo/scene.psplat"));
    CHECK(service.snapshot()[0].scene_folder == Path("fake:/scenes/Made/Trips/photo"));
    CHECK_FALSE(std::filesystem::exists(fixture.staging_root / waiting[0].id));

    // A second job waits again; cancelling it removes what it kept.
    const Path second = fixture.dir.path() / "second.jpg";
    std::filesystem::copy_file(fixture.photo, second);
    fixture.share->offline = true;
    REQUIRE(service.enqueue({second}, "fake", "", chosen.root).ok());
    REQUIRE(splat_test::wait_until([&] {
        const std::vector<splat::JobView> views = service.snapshot();
        return views.size() == 2 && views[1].state == "waiting";
    }));
    const std::string waiting_id = service.snapshot()[1].id;
    CHECK(std::filesystem::exists(staged_scene(fixture, waiting_id)));
    REQUIRE(service.cancel(waiting_id).ok());
    // Cancelled at once, or at the end of the attempt that was in flight.
    REQUIRE(splat_test::wait_until([&] { return service.snapshot()[1].state == "cancelled"; }));
    REQUIRE(splat_test::wait_until(
        [&] { return !std::filesystem::exists(fixture.staging_root / waiting_id); }));
    fixture.share->offline = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // several retry periods
    CHECK(service.snapshot()[1].state == "cancelled");
    CHECK_FALSE(fixture.share->contains("scenes/Made/second/scene.psplat"));
}
