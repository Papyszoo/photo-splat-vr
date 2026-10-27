// Helpers shared by the Spike A bench payloads (da2_bench, sharp_bench).
//
// Everything here used to be copied into both payloads: the image discovery
// (first *.jpg/*.jpeg/*.png in the bench directory), the stem/directory split
// for the output name, and the GGUF path. They are one file now so the two
// benches cannot drift. Path manipulation goes through splat::Path
// (core/common/path.h) rather than hand-written string slicing.
//
// Header-only on purpose: the payload Makefile compiles one .cpp per ELF, so a
// shared header keeps the link rule a single translation unit.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <dirent.h>

#include "common/path.h"

namespace splat {
namespace bench {

inline bool has_suffix(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// First image in `dir`, sorted by name, so a directory with photo01..03 is
// deterministic. Empty when there is none.
inline std::string first_image_in(const std::string& dir) {
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) return {};
    std::vector<std::string> names;
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (has_suffix(name, ".jpg") || has_suffix(name, ".jpeg") || has_suffix(name, ".png")) {
            names.push_back(name);
        }
    }
    closedir(handle);
    if (names.empty()) return {};
    std::sort(names.begin(), names.end());
    Path best(dir);
    best /= names.front();
    return best.string();
}

// "photo.jpg" -> "photo"; the output scene is written next to the input with
// this stem, so no extension in either case.
inline std::string stem_of(const std::string& path) {
    return Path(path).stem().string();
}

// The directory part of `path`, "." when there is none. Never returns "" so
// `dir_of(photo) + "/" + stem_of(photo)` is always a usable path.
inline std::string dir_of(const std::string& path) {
    const std::string parent = Path(path).parent_path().string();
    return parent.empty() ? std::string(".") : parent;
}

// GGUF path from PS5_MODEL_DIR (default /data/ps5-splat-vr/models).
inline std::string model_path(const char* name) {
    const char* dir = std::getenv("PS5_MODEL_DIR");
    Path base(dir != nullptr && dir[0] != '\0' ? dir : "/data/ps5-splat-vr/models");
    base /= name;
    return base.string();
}

// The SHARP GGUF a run uses: `requested` (argv[2], or the f16 default when it
// is empty) when it exists, else the f32 parity file, which is 2.6 GB but
// usable. path is empty when neither is on the console; f32_fallback says the
// second candidate was chosen so the caller can say so.
struct SharpModelFile {
    std::string path;
    bool f32_fallback = false;
};

inline SharpModelFile resolve_sharp_gguf(const std::string& requested) {
    auto exists = [](const std::string& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(Path(path), error) && !error;
    };
    const std::string want = requested.empty() ? model_path("sharp_2572gikvuh-f16-v1.gguf") : requested;
    if (exists(want)) return {want, false};
    const std::string fp32 = model_path("sharp_2572gikvuh-f32-v1.gguf");
    if (exists(fp32)) return {fp32, true};
    return {};
}

// The scene written next to the photo: <dir>/<stem><suffix>.
inline std::string output_path(const std::string& photo, const char* suffix) {
    return dir_of(photo) + "/" + stem_of(photo) + suffix;
}

}  // namespace bench
}  // namespace splat
