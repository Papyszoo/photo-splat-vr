// Small filesystem predicates shared by core/gallery.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/path.h"
#include "vfs/io.h"

namespace splat {

// True when `path` is a directory; a status error, and an unknown network
// location, count as "no". Every caller in core/gallery treats an unreadable
// entry as absent rather than failing, and a PS5 title is refused some status
// calls, so the vfs predicate (which itself never throws) is the rule.
inline bool is_dir(const Path& path) {
    vfs::FileStat status;
    return vfs::stat(path, &status).ok() && status.directory;
}

}  // namespace splat
