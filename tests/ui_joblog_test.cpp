// JobLog: the rule that turns queue snapshots into one klog line per real
// change (core/ui/job_log.h).
//
// The console's queue file outlives the process, so the rows in it at start-up
// are last session's news. The console pass printed a stale "png.png failed:
// upload failed: unknown location ..." row in every one of its ten runs, which
// reads as if the failure had just happened again. These cases pin the rule.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "ui/job_log.h"

using splat::Job;
using splat::JobLog;
using splat::JobState;
using splat::JobView;

namespace {

JobView view(const std::string& id, const std::string& name, const std::string& state) {
    JobView out;
    out.id = id;
    out.name = name;
    out.backend = "da2";
    out.state = state;
    return out;
}

Job row(const std::string& id, JobState state) {
    Job job;
    job.id = id;
    job.photo = splat::Path("/photos/" + id + ".png");
    job.backend = "da2";
    job.state = state;
    return job;
}

}  // namespace

TEST_CASE("a failure that the queue file carried in is not announced again") {
    JobLog log;
    log.seed_rows({row("job-000900", JobState::Failed), row("job-000901", JobState::Done)});

    const std::vector<JobView> after_boot = {
        view("job-000900", "png.png", "failed"), view("job-000901", "jpg.jpg", "done")};
    CHECK(log.consume(after_boot).empty());
    // ... and it stays quiet however often the service republishes.
    CHECK(log.consume(after_boot).empty());
}

TEST_CASE("a job that fails while the service runs is announced once") {
    JobLog log;
    log.seed_rows({row("job-000900", JobState::Failed)});

    std::vector<JobView> rows = {view("job-000900", "png.png", "failed")};
    rows.push_back(view("job-000902", "new.png", "running"));
    CHECK(log.consume(rows).size() == 1);

    rows[1].state = "failed";
    rows[1].message = "upload failed: smb0: cannot connect; kept at /data/ps5-splat-vr/tmp/job-000902";
    const std::vector<std::string> lines = log.consume(rows);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] ==
          "new.png failed: upload failed: smb0: cannot connect; kept at "
          "/data/ps5-splat-vr/tmp/job-000902");
    CHECK(log.consume(rows).empty());  // the same snapshot is not a new failure
}

TEST_CASE("a queued row is silent and a terminal row prints with its message") {
    JobLog log;
    CHECK(log.consume({view("a", "a.png", "queued")}).empty());
    const std::vector<std::string> lines =
        log.consume({view("b", "b.spz", "done"), view("a", "a.png", "queued")});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "b.spz done");
}

TEST_CASE("a running job prints one line per stage and per 10 % step") {
    JobLog log;
    JobView running = view("a", "a.png", "running");
    running.stage = "inferring";
    running.fraction = 0.0f;
    const std::vector<std::string> first = log.consume({running});
    REQUIRE(first.size() == 1);
    CHECK(first[0] == "a.png inferring 0% (da2)");

    running.fraction = 0.05f;  // same decile
    CHECK(log.consume({running}).empty());
    running.fraction = 0.2f;
    const std::vector<std::string> second = log.consume({running});
    REQUIRE(second.size() == 1);
    CHECK(second[0] == "a.png inferring 20% (da2)");
    running.fraction = 0.2f;
    running.stage = "writing scene";
    const std::vector<std::string> third = log.consume({running});
    REQUIRE(third.size() == 1);
    CHECK(third[0] == "a.png writing scene 20% (da2)");
}

TEST_CASE("a second job at the stage and step the first ended on still prints") {
    // The dedupe is per job: a conversion that stops publishing at
    // "writing scene 90%" and an import that reaches the same stage and step
    // must not be mistaken for a repeat of the first job's line.
    JobLog log;
    JobView first = view("job-1", "png.png", "running");
    first.stage = "writing scene";
    first.fraction = 0.9f;
    REQUIRE(log.consume({first}).size() == 1);
    first.state = "done";
    REQUIRE(log.consume({first}).size() == 1);  // "png.png done"

    JobView second = view("job-2", "tiny.spz", "running");
    second.stage = "writing scene";
    second.fraction = 0.9f;
    const std::vector<std::string> lines = log.consume({first, second});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "tiny.spz writing scene 90% (da2)");
}

TEST_CASE("the console's stale row is exactly the case the seed covers") {
    // The console pass's jobs.json, abridged: one failed upload from an earlier
    // boot beside the perf rows the last runs left behind.
    JobLog log;
    log.seed_rows({row("job-000900", JobState::Failed), row("job-000901", JobState::Done),
                   row("job-000902", JobState::Cancelled)});
    const std::vector<JobView> boot = {view("job-000900", "png.png", "failed"),
                                       view("job-000901", "jpg.jpg", "done"),
                                       view("job-000902", "bmp.bmp", "cancelled")};
    CHECK(log.consume(boot).empty());
}
