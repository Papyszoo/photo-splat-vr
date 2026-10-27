#pragma once

// Locates the external data directory (AGENTS.md): `$SPLAT_DATA_DIR` when it is
// set, otherwise the repository's parent directory. Golden tensors, reference
// .ply files and model weights live there, never in the repo, so the tests that
// need them skip when the file is absent.
//
// Nothing here may contain an absolute personal path: the path is derived from
// this file's location, which CMake passes to the compiler.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace splat_test {

inline std::filesystem::path repo_root() {
    // .../<repo>/tests/data_dir.h
    const std::filesystem::path here(__FILE__);
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(here, error);
    if (error) return here.parent_path().parent_path();
    return canonical.parent_path().parent_path();
}

inline std::filesystem::path data_dir() {
    const char* from_env = std::getenv("SPLAT_DATA_DIR");
    if (from_env != nullptr && from_env[0] != '\0') return std::filesystem::path(from_env);
    return repo_root().parent_path();
}

// e.g. data_path("golden/photo01_living_room/reference.ply").
inline std::filesystem::path data_path(const std::string& relative) {
    return data_dir() / std::filesystem::path(relative);
}

}  // namespace splat_test
