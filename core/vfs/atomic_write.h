// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "common/path.h"

namespace splat::vfs {

// Replaces the local file `path` with `text` in one step: the bytes go to a
// temporary beside it, are flushed to the device, and are renamed over the
// destination (through the VFS, like every other write) with the folder entry
// flushed too, so the rename survives a power cut and not only a process crash.
// The temporary is removed when anything fails, so a reader never sees half a
// file.
//
// This is the one atomic text write: JobQueue's state file
// (core/jobs/job_queue.cpp), a scene's meta.json (core/scene/scene_view.cpp)
// and the visibility/turn state files all use it. A network path has its own
// adapter's write instead (core/vfs); this helper creates no folder, so the
// caller makes the parent first.
//
// False with `error` set when the write, the rename or the folder flush
// failed. `mode`, when not 0, is the permission the finished file has (the
// visible-folders state file is 0600); it is applied to the temporary before
// the rename, so the destination is never briefly readable by everyone.
bool write_text_atomically(const Path& path, const std::string& text, std::string* error,
                           int mode = 0);

}  // namespace splat::vfs
