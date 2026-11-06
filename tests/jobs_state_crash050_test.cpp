// The 0.5.0 dev title's launch crash (REPORT-CR.md), from the two sides the
// crash touched: a job queue read from a real console state file, and a state
// file that cannot be believed.
//
// The real file is tests/fixtures/crash-050/jobs.json, copied read-only from the
// dev title's /data/ps5-splat-vr/state/jobs.json on the console (10 done rows,
// next_sequence 911, the pre-stage-H shape with no "source_folder" key). It holds
// no credential of any kind - job rows name photos under /data/ps5-splat-vr and
// the scene folders they produced - so nothing needed masking; the state files
// that do carry one (locations.json) do not exist on this install.
//
// The crash itself was not a bad file: it was a stale object linked against the
// new headers (sizeof(Job) 208 vs 232, sizeof(JobService) 224 vs 488), and the
// JobService constructor wrote 264 bytes past the block the object had
// allocated. These tests hold the other end: the file loads, the count is
// plausible, the worker publishes it, and an unbelievable file is refused
// instead of walked.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "jobs/gpu_phase_guard.h"
#include "jobs/job_queue.h"
#include "jobs/job_runner.h"
#include "test_util.h"
#include "ui/job_service.h"

using splat::JobQueue;
using splat::JobService;
using splat::JobState;
using splat::JobView;

namespace {

// tests/fixtures/crash-050/jobs.json, beside this file (tests/data_dir.h's
// repo_root() is the repo; the fixture lives in the repo, not in $SPLAT_DATA_DIR).
splat::Path console_queue_fixture() {
    const std::filesystem::path here(__FILE__);
    return here.parent_path() / "fixtures" / "crash-050" / "jobs.json";
}

std::string read_all(const splat::Path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const splat::Path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// The console's file in a temp state folder, exactly as the title would find it.
splat::Path install_console_queue(const splat_test::TempDir& dir) {
    const splat::Path file = dir.path() / "state" / "jobs.json";
    const std::string text = read_all(console_queue_fixture());
    REQUIRE_MESSAGE(!text.empty(), "missing fixture " << console_queue_fixture().c_str());
    write_all(file, text);
    return file;
}

}  // namespace

TEST_CASE("jobs: the console's real 0.5.0 state file loads and publishes") {
    splat_test::TempDir dir("crash050-state");
    const splat::Path file = install_console_queue(dir);

    // The reader the title boots through, then the same validation the log line
    // and every allocation after it rely on.
    const auto opened = JobQueue::open(file);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    JobQueue queue = opened.value();
    REQUIRE(queue.jobs().size() == 10);
    CHECK(queue.jobs().size() <= JobQueue::kMaxJobs);

    // Every row is a finished conversion the console wrote; nothing is left
    // running, and the sequence continues where the console stopped (911).
    for (const splat::Job& job : queue.jobs()) {
        CHECK(job.kind == splat::JobKind::convert);
        CHECK(job.state == JobState::Done);
        CHECK(job.album == "00_v02_console");
        CHECK(job.backend == "da2");
        CHECK(job.id.rfind("job-", 0) == 0);
        // The pre-stage-H file has no "source_folder": the row and the scene's
        // meta.json keep what they had instead of a guessed key.
        CHECK(job.source_folder.empty());
        CHECK(job.rotate_quarters == 0);
    }
    CHECK(queue.jobs().front().id == "job-000901");
    CHECK(queue.jobs().back().id == "job-000910");
    CHECK(queue.jobs().back().sequence == 910);

    // The next enqueue continues the console's numbering rather than reusing an
    // id (911 is the file's next_sequence).
    const auto next = queue.enqueue(dir.path() / "after-crash.jpg", "da2", "00_v02_console");
    REQUIRE_MESSAGE(next.ok(), next.message());
    CHECK(next.value() == "job-000911");

    // The real path the crash died in: JobService's worker thread publishing
    // queue_.jobs() into JobView rows. A layout mismatch here is a heap
    // corruption the sanitizer build turns into a failure, not a silent free().
    splat::GpuPhaseGuard guard;
    std::atomic<int> publishes{0};
    splat::JobRunner runner(
        queue, guard,
        [](const std::string&) -> std::unique_ptr<splat::IReconstructor> { return nullptr; },
        splat::JobRunnerConfig{dir.path() / "scenes", dir.path() / "model.gguf", 64, 90});
    JobService service(queue, runner, [&publishes] { ++publishes; });

    REQUIRE(splat_test::wait_until([&publishes] { return publishes.load() > 0; }));

    const std::vector<JobView> views = service.snapshot();
    REQUIRE(views.size() == 11);  // the ten console rows plus the one just queued
    CHECK(views.front().id == "job-000901");
    CHECK(views.front().state == "done");
    CHECK(views.front().kind == "convert");
    // The queued row is the worker's to run: it fails on a photo that is not
    // there, so its state may be queued, running or failed by now, never done.
    CHECK(views.back().id == "job-000911");
    CHECK(views.back().state != "done");
    // A published row carries the fields the queue screen filters and opens on.
    CHECK(views.front().album == "00_v02_console");
    CHECK(views.front().scene_folder.empty() == false);
}

TEST_CASE("jobs: the writer and the reader agree on the queue limits") {
    splat_test::TempDir dir("crash050-limits");
    const splat::Path file = dir.path() / "state" / "jobs.json";

    // A row at the end of the id space: a row's sequence stays below the limit,
    // so the file's next_sequence may be the limit and still reload. The old
    // reader accepted a row at exactly the limit, then wrote the limit + 1 and
    // refused its own file on the next boot.
    write_all(file,
              "{\"jobs\":[{\"id\":\"job-999999998\",\"photo\":\"/a.jpg\","
              "\"state\":\"done\",\"sequence\":999999998}],\"next_sequence\":999999999}");
    const auto opened = JobQueue::open(file);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    JobQueue queue = opened.value();
    const auto last = queue.enqueue(dir.path() / "last.jpg", "da2");
    REQUIRE_MESSAGE(last.ok(), last.message());
    CHECK(last.value() == "job-999999999");
    // The file save() just wrote is one open() accepts: a restart keeps it.
    const auto reopened = JobQueue::open(file);
    REQUIRE_MESSAGE(reopened.ok(), reopened.message());
    CHECK(reopened.value().jobs().size() == 2);
    // The id space is exhausted now: the next enqueue is refused here, not
    // written into a file the next boot would refuse.
    JobQueue exhausted_queue = reopened.value();
    const auto exhausted = exhausted_queue.enqueue(dir.path() / "next.jpg", "da2");
    REQUIRE_FALSE(exhausted.ok());
    CHECK(exhausted.message().find("id space is exhausted") != std::string::npos);

    // A row past the end of the id space is not a row this title wrote.
    write_all(file,
              "{\"jobs\":[{\"id\":\"job-1000000000\",\"photo\":\"/a.jpg\","
              "\"state\":\"done\",\"sequence\":1000000000}],\"next_sequence\":1000000001}");
    const auto beyond = JobQueue::open(file);
    REQUIRE_FALSE(beyond.ok());
    CHECK(beyond.message().find("not below") != std::string::npos);

    // The row limit is the writer's too. A queue at the limit (the count open()
    // accepts) refuses the next row, and the readable file stays in place
    // instead of being replaced by one the boot would discard.
    std::string full = "{\"jobs\":[";
    for (std::size_t i = 0; i < JobQueue::kMaxJobs; ++i) {
        if (i != 0) full += ",";
        full += "{\"id\":\"job-" + std::to_string(i + 1) + "\",\"photo\":\"/a" +
                std::to_string(i) + ".jpg\",\"state\":\"done\",\"sequence\":" +
                std::to_string(i + 1) + "}";
    }
    full += "],\"next_sequence\":" + std::to_string(JobQueue::kMaxJobs + 1) + "}";
    write_all(file, full);
    const auto at_limit = JobQueue::open(file);
    REQUIRE_MESSAGE(at_limit.ok(), at_limit.message());
    CHECK(at_limit.value().jobs().size() == JobQueue::kMaxJobs);
    JobQueue full_queue = at_limit.value();
    const auto too_many = full_queue.enqueue(dir.path() / "over.jpg", "da2");
    REQUIRE_FALSE(too_many.ok());
    CHECK(too_many.message().find("queue is full") != std::string::npos);
    const auto still_readable = JobQueue::open(file);
    REQUIRE_MESSAGE(still_readable.ok(), still_readable.message());
    CHECK(still_readable.value().jobs().size() == JobQueue::kMaxJobs);
}

TEST_CASE("jobs: a truncated state file is refused, kept, and starts an empty queue") {
    splat_test::TempDir dir("crash050-truncated");
    const splat::Path file = dir.path() / "state" / "jobs.json";
    const std::string whole = read_all(console_queue_fixture());
    REQUIRE(whole.size() > 200);
    // A write that stopped halfway: valid JSON up to a point, no closing braces.
    write_all(file, whole.substr(0, whole.size() / 2));

    const auto opened = JobQueue::open(file);
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.message().find("invalid job queue JSON") != std::string::npos);

    std::string notice;
    const JobQueue queue = JobQueue::open_or_empty(file, &notice);
    CHECK(queue.jobs().empty());
    CHECK_FALSE(notice.empty());
    // The evidence is kept for the operator, and the empty queue owns the name
    // the next save writes.
    CHECK(std::filesystem::exists(file.string() + ".rejected"));
    CHECK(std::filesystem::exists(file) == false);
    CHECK(queue.state_file() == file);
    CHECK(notice.find("kept as") != std::string::npos);
}

TEST_CASE("jobs: an implausible row count or size is refused before it is walked") {
    splat_test::TempDir dir("crash050-implausible");
    const splat::Path file = dir.path() / "state" / "jobs.json";

    // The shape that printed 1.1e19 rows: a jobs array far past any real queue.
    // One row per element, so the file itself stays small; the count is what the
    // reader must refuse before it builds anything.
    std::string many = "{\"jobs\":[";
    for (std::size_t i = 0; i <= JobQueue::kMaxJobs; ++i) {
        if (i != 0) many += ",";
        many += "{\"id\":\"job-1\",\"photo\":\"/a.jpg\",\"state\":\"queued\"}";
    }
    many += "],\"next_sequence\":2}";
    write_all(file, many);
    const auto refused = JobQueue::open(file);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.message().find("row limit") != std::string::npos);

    // A file larger than the byte limit is refused on its stat alone.
    const splat::Path huge = dir.path() / "state" / "huge.json";
    std::filesystem::create_directories(huge.parent_path());
    {
        std::ofstream out(huge, std::ios::binary | std::ios::trunc);
        const std::string block(1 << 16, 'x');
        for (std::size_t i = 0; i <= JobQueue::kMaxFileBytes / block.size(); ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    const auto too_big = JobQueue::open(huge);
    REQUIRE_FALSE(too_big.ok());
    CHECK(too_big.message().find("byte limit") != std::string::npos);

    // A field that is not the type it claims (a rotation of 99, a duration that
    // is not a number) is refused by name rather than used.
    write_all(file,
              "{\"jobs\":[{\"id\":\"job-000001\",\"photo\":\"/a.jpg\",\"state\":\"queued\","
              "\"rotate\":99}],\"next_sequence\":2}");
    const auto bad_rotate = JobQueue::open(file);
    REQUIRE_FALSE(bad_rotate.ok());
    CHECK(bad_rotate.message().find("outside -3..3") != std::string::npos);

    write_all(file,
              "{\"jobs\":[{\"id\":\"job-000001\",\"photo\":\"/a.jpg\",\"state\":\"queued\","
              "\"seconds_taken\":-5}],\"next_sequence\":2}");
    const auto bad_duration = JobQueue::open(file);
    REQUIRE_FALSE(bad_duration.ok());
    CHECK(bad_duration.message().find("implausible duration") != std::string::npos);

    // And the boot path still comes up: empty queue, one notice, no throw.
    std::string notice;
    const JobQueue queue = JobQueue::open_or_empty(file, &notice);
    CHECK(queue.jobs().empty());
    CHECK_FALSE(notice.empty());
}
