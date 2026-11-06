// Upload-then-delete (L3-C): a conversion whose album is on a network location
// is built in <staging_root>/<job id>, copied to the share, verified and then
// removed; a failed upload keeps the staging folder and names it in the job's
// message. The destination is an in-memory fake registered as "fake".
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "jobs/gpu_phase_guard.h"
#include "jobs/image_io.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "jobs/staging.h"
#include "recon/reconstructor.h"
#include "test_util.h"
#include "vfs/io.h"

using splat::Path;

namespace {

// The destination share, in memory. `fail_writes` makes every write fail with
// the fake's own message, which is what a dropped connection looks like to the
// runner.
class FakeShare : public splat::vfs::IFileSystem {
public:
    std::string id() const override { return "fake"; }
    std::string describe() const override { return "fake:/"; }
    bool writable() const override { return true; }

    splat::Result<std::vector<uint8_t>> read(const std::string& relative,
                                             size_t) override {
        const auto found = files.find(relative);
        if (found == files.end()) {
            return splat::Result<std::vector<uint8_t>>::failure("fake: no such file: " + relative);
        }
        return splat::Result<std::vector<uint8_t>>::success(found->second);
    }
    splat::Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        if (fail_writes) return splat::Status::error("fake share refused the write");
        files[relative] = std::vector<uint8_t>(data, data + size);
        return splat::Status::success();
    }
    splat::Status list(const std::string& relative, std::vector<splat::vfs::DirEntry>* entries) override {
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
        for (auto it = files.begin(); it != files.end();) {
            it = under(relative, it->first) ? files.erase(it) : std::next(it);
        }
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

    splat::JobRunner runner() {
        splat::JobRunnerConfig config;
        config.output_root = scenes_root;
        config.staging_root = staging_root;
        config.model_path = dir.path() / "model.gguf";
        config.thumbnail_max_edge = 128;
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
