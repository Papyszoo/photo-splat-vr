// SPDX-License-Identifier: GPL-3.0-or-later

#include "jobs/staging.h"

#include <algorithm>
#include <utility>

#include "vfs/io.h"

namespace splat {
namespace {

// The states whose staging folder is still the scene's only copy.
bool job_may_need(const std::vector<Job>& jobs, const std::string& name) {
    for (const Job& job : jobs) {
        if (job.id != name) continue;
        return job.state == JobState::Failed || job.state == JobState::Queued ||
               job.state == JobState::Running;
    }
    return false;  // no job owns this folder
}

}  // namespace

std::vector<std::string> sweep_staging(const Path& staging_root, const std::vector<Job>& jobs) {
    std::vector<std::string> removed;
    if (staging_root.empty()) return removed;

    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(staging_root, &entries).ok()) return removed;

    for (const vfs::DirEntry& entry : entries) {
        if (!entry.directory || entry.symlink) continue;
        if (job_may_need(jobs, entry.name)) continue;
        const Path folder = staging_root / entry.name;
        if (vfs::remove_tree(folder).ok()) removed.push_back(entry.name);
    }
    std::sort(removed.begin(), removed.end());
    return removed;
}

}  // namespace splat
