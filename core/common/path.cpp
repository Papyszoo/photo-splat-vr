// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/path.h"

#include <cctype>
#include <system_error>

namespace splat {

bool normal_absolute(const Path& path, Path& out) {
    if (path.empty()) return false;
    std::error_code error;
    out = path;
    if (!out.is_absolute()) {
        const Path base = std::filesystem::current_path(error);
        if (error) return false;
        out = base / out;
    }
    out = out.lexically_normal();
    // lexically_normal() keeps a trailing separator as an empty last element;
    // drop it so "<root>/album/scene/" is the path without the slash.
    while (out.filename().empty() && out != out.parent_path()) out = out.parent_path();
    return out.is_absolute();
}

std::string lowercase_extension(const Path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

}  // namespace splat
