// Boot-time staging hygiene for upload-then-delete (L3-C).
//
// A conversion into a network location is built in <staging_root>/<job id>,
// uploaded and then deleted. Two kinds of folder can survive that:
//
//   * a folder whose job is Done or Cancelled - the delete step failed or the
//     process died right after the upload. Nothing can use it again.
//   * a folder that names no job in the queue at all - a crash between the
//     write and the queue save, or a queue file that was reset.
//
// Both are swept at start-up. A Failed job's folder is *kept*: it is the only
// copy of that scene, its message tells the user where it is, and the next run
// of the job deletes it itself ("a job whose upload fails keeps its local
// result"). A Queued or Running job's folder stays because the runner is about
// to use it, and a Waiting job's folder is the finished scene its retry will
// upload.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "common/path.h"
#include "jobs/job_queue.h"

namespace splat {

// Removes what no live job can need and returns the names it removed (for one
// log line). Best effort: an unreadable staging folder returns an empty list
// rather than failing a boot.
std::vector<std::string> sweep_staging(const Path& staging_root, const std::vector<Job>& jobs);

}  // namespace splat
