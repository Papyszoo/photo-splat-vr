// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/job_log.h"

#include <cmath>
#include <utility>

namespace splat {

void JobLog::seed(const std::vector<JobView>& views) {
    for (const JobView& view : views) states_[view.id] = view.state;
}

void JobLog::seed_rows(const std::vector<Job>& jobs) {
    for (const Job& job : jobs) states_[job.id] = job_state_name(job.state);
}

std::vector<std::string> JobLog::consume(const std::vector<JobView>& views) {
    std::vector<std::string> lines;
    for (const JobView& view : views) {
        std::string& known = states_[view.id];
        if (view.state == "running") {
            // One line per stage and per 10 % step: a conversion publishes far
            // more often than that, and a klog line per frame would drown the
            // console log.
            const int decile = static_cast<int>(view.fraction * 10.0f);
            Progress& seen = progress_[view.id];
            if (view.stage == seen.stage && decile == seen.decile) continue;
            seen.stage = view.stage;
            seen.decile = decile;
            lines.push_back(view.name + " " + view.stage + " " + std::to_string(decile * 10) +
                            "% (" + view.backend + ")");
            continue;
        }
        progress_.erase(view.id);
        if (known == view.state) continue;
        known = view.state;
        // "queued" is the state a row is created in; printing it would announce
        // every job twice (queued, then running).
        if (view.state == "queued") continue;
        lines.push_back(view.name + " " + view.state +
                        (view.message.empty() ? std::string() : ": " + view.message));
    }
    return lines;
}

}  // namespace splat
