// The two small filesystem helpers the model code shares: the source file's
// base name (it goes into the GGUF metadata) and "delete if it exists" (partial
// outputs, a legacy .pt, the inbox copy). One copy each, so the file converter,
// the DA2 converter, the manager and the streaming converter agree.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#include "common/path.h"

namespace splat {

inline std::string basename_of(const Path& path) { return path.filename().string(); }

// Best effort: a file that cannot be removed is not an error here. Callers that
// need to know check the result of their own operation.
inline void remove_if_present(const Path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace splat
