// Small filesystem predicates shared by core/gallery.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <system_error>

#include "common/path.h"

namespace splat {

// True when `path` is a directory; a status error counts as "no". Every caller
// in core/gallery treats an unreadable entry as absent rather than failing, and
// a PS5 title is refused some status calls, so the error code form is the rule.
inline bool is_dir(const Path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

}  // namespace splat
