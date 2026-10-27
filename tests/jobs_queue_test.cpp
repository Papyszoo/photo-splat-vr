#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "jobs/job_queue.h"
#include "test_util.h"

using splat::JobQueue;
using splat::JobState;

namespace {

splat::Path queue_file(const splat_test::TempDir& dir) {
    return dir.path() / "state" / "jobs.json";
}

void write_text(const splat::Path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace

TEST_CASE("jobs: queue keeps enqueued jobs across a restart") {
    splat_test::TempDir dir("jobs-queue");
    const splat::Path file = queue_file(dir);

    const auto opened = JobQueue::open(file);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    JobQueue queue = opened.value();
    CHECK(queue.jobs().empty());

    const auto id = queue.enqueue(dir.path() / "photo.jpg", "sharp");
    REQUIRE_MESSAGE(id.ok(), id.message());
    CHECK(id.value() == "job-000001");

    const auto reloaded = JobQueue::open(file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    REQUIRE(reloaded.value().jobs().size() == 1);
    const splat::Job& job = reloaded.value().jobs().front();
    CHECK(job.id == id.value());
    CHECK(job.state == JobState::Queued);
    CHECK(job.backend == "sharp");
    CHECK(job.album.empty());
    CHECK(job.photo.filename() == "photo.jpg");
    CHECK(job.sequence == 1);

    // The sequence continues after a restart instead of reusing ids.
    JobQueue second = reloaded.value();
    const auto next = second.enqueue(dir.path() / "other.jpg", "sharp");
    REQUIRE_MESSAGE(next.ok(), next.message());
    CHECK(next.value() == "job-000002");
}

TEST_CASE("jobs: a job left running is re-queued on load") {
    splat_test::TempDir dir("jobs-restart");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto id = queue.enqueue(dir.path() / "photo.jpg", "sharp");
    REQUIRE(id.ok());
    const auto taken = queue.take_next();
    REQUIRE_MESSAGE(taken.ok(), taken.message());
    REQUIRE(taken.value().has_value());
    REQUIRE(queue.find(id.value()) != nullptr);
    CHECK(queue.find(id.value())->state == JobState::Running);

    const auto restarted = JobQueue::open(file);
    REQUIRE_MESSAGE(restarted.ok(), restarted.message());
    const splat::Job* job = restarted.value().find(id.value());
    REQUIRE(job != nullptr);
    CHECK(job->state == JobState::Queued);

    // And it can be taken again after the restart.
    JobQueue resumed = restarted.value();
    const auto resumed_taken = resumed.take_next();
    REQUIRE(resumed_taken.ok());
    REQUIRE(resumed_taken.value().has_value());
    CHECK(resumed_taken.value().value().id == id.value());
}

TEST_CASE("jobs: only one job runs at a time") {
    splat_test::TempDir dir("jobs-serial");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto first = queue.enqueue(dir.path() / "a.jpg", "sharp");
    const auto second = queue.enqueue(dir.path() / "b.jpg", "sharp");
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    const auto taken = queue.take_next();
    REQUIRE(taken.ok());
    REQUIRE(taken.value().has_value());
    CHECK(taken.value().value().id == first.value());

    const auto blocked = queue.take_next();
    REQUIRE(blocked.ok());
    CHECK_FALSE(blocked.value().has_value());

    const splat::Status done = queue.mark_done(first.value(), dir.path() / "scenes" / first.value(), 4.5);
    REQUIRE_MESSAGE(done.ok(), done.message());

    const auto next = queue.take_next();
    REQUIRE(next.ok());
    REQUIRE(next.value().has_value());
    CHECK(next.value().value().id == second.value());
}

TEST_CASE("jobs: done and failed jobs persist their result") {
    splat_test::TempDir dir("jobs-results");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto done_id = queue.enqueue(dir.path() / "ok.jpg", "sharp");
    const auto failed_id = queue.enqueue(dir.path() / "bad.jpg", "sharp");
    REQUIRE(done_id.ok());
    REQUIRE(failed_id.ok());

    REQUIRE(queue.take_next().value().has_value());
    const splat::Path folder = dir.path() / "scenes" / done_id.value();
    REQUIRE_MESSAGE(queue.mark_done(done_id.value(), folder, 12.5).ok(), "mark_done failed");

    REQUIRE(queue.take_next().value().has_value());
    REQUIRE_MESSAGE(queue.mark_failed(failed_id.value(), "decoder said no").ok(), "mark_failed failed");

    const auto reloaded = JobQueue::open(file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    const splat::Job* done = reloaded.value().find(done_id.value());
    REQUIRE(done != nullptr);
    CHECK(done->state == JobState::Done);
    CHECK(done->scene_folder == folder);
    CHECK(done->seconds_taken == doctest::Approx(12.5));

    const splat::Job* failed = reloaded.value().find(failed_id.value());
    REQUIRE(failed != nullptr);
    CHECK(failed->state == JobState::Failed);
    CHECK(failed->message == "decoder said no");
}

TEST_CASE("jobs: a job remembers its album and an old queue file still loads") {
    splat_test::TempDir dir("jobs-album");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto in_album = queue.enqueue(dir.path() / "album.jpg", "sharp", "2026-10-26");
    const auto at_root = queue.enqueue(dir.path() / "root.jpg", "sharp");
    REQUIRE(in_album.ok());
    REQUIRE(at_root.ok());

    const auto reloaded = JobQueue::open(file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    REQUIRE(reloaded.value().find(in_album.value()) != nullptr);
    CHECK(reloaded.value().find(in_album.value())->album == "2026-10-26");
    CHECK(reloaded.value().find(at_root.value())->album.empty());

    // A queue file written before albums existed has no "album" key: it loads
    // with the loose-scene album instead of failing.
    write_text(file,
               "{\n"
               "  \"version\": 1,\n"
               "  \"next_sequence\": 2,\n"
               "  \"jobs\": [\n"
               "    {\"id\": \"job-000001\", \"photo\": \"/tmp/a.jpg\", \"backend\": \"sharp\",\n"
               "     \"state\": \"queued\", \"message\": \"\", \"scene_folder\": \"\",\n"
               "     \"seconds_taken\": 0.0, \"sequence\": 1}\n"
               "  ]\n"
               "}\n");
    const auto old = JobQueue::open(file);
    REQUIRE_MESSAGE(old.ok(), old.message());
    REQUIRE(old.value().jobs().size() == 1);
    CHECK(old.value().jobs()[0].album.empty());
    CHECK(old.value().jobs()[0].state == JobState::Queued);
    CHECK(old.value().jobs()[0].backend == "sharp");
}

TEST_CASE("jobs: cancel stops queued and running jobs") {
    splat_test::TempDir dir("jobs-cancel");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto first = queue.enqueue(dir.path() / "first.jpg", "sharp");
    const auto second = queue.enqueue(dir.path() / "second.jpg", "sharp");
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    // first becomes running, second stays queued.
    REQUIRE_MESSAGE(queue.take_next().value().has_value(), "take next");
    REQUIRE_MESSAGE(queue.cancel(first.value()).ok(), "cancel running");
    CHECK(queue.find(first.value())->state == JobState::Cancelled);

    REQUIRE_MESSAGE(queue.cancel(second.value()).ok(), "cancel queued");
    CHECK(queue.find(second.value())->state == JobState::Cancelled);
    CHECK(queue.find(second.value())->message == "cancelled");

    // A cancelled job never becomes done and is not handed out again.
    CHECK_FALSE(queue.mark_done(first.value(), dir.path() / "scenes", 1.0).ok());
    const auto none = queue.take_next();
    REQUIRE(none.ok());
    CHECK_FALSE(none.value().has_value());

    auto reloaded = JobQueue::open(file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    CHECK(reloaded.value().find(first.value())->state == JobState::Cancelled);
    CHECK(reloaded.value().find(second.value())->state == JobState::Cancelled);
    CHECK_FALSE(reloaded.value().cancel(first.value()).ok());
}

TEST_CASE("jobs: a photo is not enqueued twice while it is active") {
    splat_test::TempDir dir("jobs-dup");
    const splat::Path file = queue_file(dir);

    JobQueue queue = JobQueue::open(file).value();
    const auto first = queue.enqueue(dir.path() / "photo.jpg", "sharp");
    REQUIRE(first.ok());

    const auto duplicate = queue.enqueue(dir.path() / "./photo.jpg", "sharp");
    CHECK_FALSE(duplicate.ok());
    CHECK(duplicate.message().find("already queued") != std::string::npos);
    CHECK(queue.jobs().size() == 1);

    const auto taken = queue.take_next();
    REQUIRE(taken.ok());
    CHECK_FALSE(queue.enqueue(dir.path() / "photo.jpg", "sharp").ok());

    REQUIRE_MESSAGE(queue.mark_done(first.value(), dir.path() / "scenes", 1.0).ok(), "mark done");
    const auto again = queue.enqueue(dir.path() / "photo.jpg", "sharp");
    CHECK_MESSAGE(again.ok(), "the same photo can be converted again after a done job");
    CHECK(queue.jobs().size() == 2);
}

TEST_CASE("jobs: enqueue stores an absolute, lexically normal photo path") {
    splat_test::TempDir dir("jobs-normal");
    const splat::Path file = queue_file(dir);
    JobQueue queue = JobQueue::open(file).value();
    const splat::Path cwd = std::filesystem::current_path();

    // A relative path with "./" and ".." parts is made absolute and normalised
    // without touching the filesystem (a PS5 title is refused realpath()).
    const auto id = queue.enqueue(splat::Path("./photos/../photos/a.jpg"), "sharp");
    REQUIRE_MESSAGE(id.ok(), id.message());
    const splat::Job* job = queue.find(id.value());
    REQUIRE(job != nullptr);
    CHECK(job->photo.is_absolute());
    CHECK(job->photo == (cwd / "photos" / "a.jpg").lexically_normal());
    CHECK(job->photo.string().find("./") == std::string::npos);
    CHECK(job->photo.string().find("..") == std::string::npos);

    // The saved file carries the same path.
    const auto reloaded = JobQueue::open(file);
    REQUIRE_MESSAGE(reloaded.ok(), reloaded.message());
    REQUIRE(reloaded.value().find(id.value()) != nullptr);
    CHECK(reloaded.value().find(id.value())->photo == job->photo);

    // Nothing to normalise is a refusal, not an empty job.
    const auto empty = queue.enqueue(splat::Path(), "sharp");
    CHECK_FALSE(empty.ok());
    CHECK(empty.message().find("empty") != std::string::npos);
}

TEST_CASE("jobs: an unreadable queue file is an error, not a silent reset") {
    splat_test::TempDir dir("jobs-corrupt");
    const splat::Path file = queue_file(dir);
    write_text(file, "{ this is not json");

    const auto opened = JobQueue::open(file);
    CHECK_FALSE(opened.ok());
    CHECK(opened.message().find("jobs.json") != std::string::npos);
}

TEST_CASE("jobs: take_next on an empty queue returns nothing") {
    splat_test::TempDir dir("jobs-empty");
    JobQueue queue = JobQueue::open(queue_file(dir)).value();
    const auto taken = queue.take_next();
    REQUIRE(taken.ok());
    CHECK_FALSE(taken.value().has_value());
}

TEST_CASE("jobs: every save replaces the state file atomically") {
    splat_test::TempDir dir("jobs-atomic");
    const splat::Path file = queue_file(dir);
    JobQueue queue = JobQueue::open(file).value();

    std::error_code error;
    REQUIRE_MESSAGE(queue.enqueue(dir.path() / "a.jpg", "sharp").ok(), "enqueue");
    REQUIRE_MESSAGE(queue.save().ok(), "save");
    CHECK(std::filesystem::exists(file, error));

    // The temporary file must not survive a successful save: a stale .tmp is
    // what makes an operator think a crash happened.
    splat::Path temporary = file;
    temporary += ".tmp";
    CHECK_FALSE(std::filesystem::exists(temporary, error));

    // The file is complete JSON, not a prefix of it.
    {
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        CHECK(text.front() == '{');
        CHECK(text.back() == '\n');
        CHECK(text.find("\"jobs\"") != std::string::npos);
    }

    // And it reloads to the same queue.
    const auto reopened = JobQueue::open(file);
    REQUIRE_MESSAGE(reopened.ok(), reopened.message());
    REQUIRE(reopened.value().jobs().size() == 1);
    CHECK(reopened.value().jobs()[0].photo.filename().string() == "a.jpg");
}

TEST_CASE("jobs: a job interrupted mid-run is re-queued, not left running") {
    // What a power cut during inference looks like: the state file says
    // "running" and no process is left to finish it.
    splat_test::TempDir dir("jobs-crash");
    const splat::Path file = queue_file(dir);
    write_text(file,
               "{\n"
               "  \"version\": 1,\n"
               "  \"next_sequence\": 2,\n"
               "  \"jobs\": [\n"
               "    {\"id\": \"job-000001\", \"photo\": \"/tmp/a.jpg\", \"backend\": \"sharp\",\n"
               "     \"state\": \"running\", \"message\": \"\", \"scene_folder\": \"\",\n"
               "     \"seconds_taken\": 0.0, \"sequence\": 1}\n"
               "  ]\n"
               "}\n");
    const auto opened = JobQueue::open(file);
    REQUIRE_MESSAGE(opened.ok(), opened.message());
    JobQueue queue = opened.value();
    REQUIRE(queue.jobs().size() == 1);
    CHECK(queue.jobs()[0].state == JobState::Queued);

    // The runner must be able to pick it up again.
    const auto taken = queue.take_next();
    REQUIRE(taken.ok());
    REQUIRE(taken.value().has_value());
    const splat::Job* running = queue.find(taken.value()->id);
    REQUIRE(running != nullptr);
    CHECK(running->state == JobState::Running);
}
