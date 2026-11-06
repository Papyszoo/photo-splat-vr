// Turning job-queue snapshots into log lines: one line per real change.
//
// The console's queue is a file, and the rows in it outlive the process that
// wrote them. A host that printed every row on its first snapshot re-announced
// the last session's failure at every boot ("png.png failed: upload failed:
// unknown location ..."), which reads as if it happened again. The rule is: a
// row that already existed when the service started is history, and only a
// change observed while the service runs is news.
//
// This is the decision, not the printing: the host passes the snapshot from
// IJobService::snapshot() and prints the returned lines, so the rule can be
// tested without a console (tests/ui_joblog_test.cpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

#include "jobs/job_queue.h"
#include "ui/services.h"

namespace splat {

class JobLog {
public:
    // Marks every row in `views` as already known and prints nothing. Call it
    // with the service's first snapshot (JobService::snapshot() right after
    // construction): those rows come from the queue file, not from this run.
    void seed(const std::vector<JobView>& views);

    // The same, from the queue rows loaded out of the file. A host uses this
    // one *before* it constructs the job service, so a row that the file
    // carried in cannot be printed by the worker's very first publish - there
    // is no window in which a stale failure looks new.
    void seed_rows(const std::vector<Job>& jobs);

    // The lines this snapshot adds, in queue order. Empty when nothing changed
    // (a repeated snapshot, a queued row, the same stage and decile of a
    // running one). A running job prints at most one line per stage and per
    // 10 % step; a terminal row prints once.
    std::vector<std::string> consume(const std::vector<JobView>& views);

private:
    // The last stage/decile printed *per job*. One pair for the whole log
    // dropped the next job's line whenever it reached the stage and decile the
    // previous job ended on ("writing scene 90%"), which is every import that
    // follows a conversion.
    struct Progress {
        std::string stage;
        int decile = -1;
    };
    std::map<std::string, Progress> progress_;   // job id -> last logged progress
    std::map<std::string, std::string> states_;  // job id -> last logged state
};

}  // namespace splat
