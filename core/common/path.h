#pragma once

#include <filesystem>

namespace splat {

// The C2 interface uses `Path` for model files and scene folders. Keeping the
// alias in one place lets the console build swap in a different filesystem
// implementation without touching the interfaces.
using Path = std::filesystem::path;

// Absolute, lexically normal form of `path`: a relative path is resolved
// against the working directory, then "." and ".." are resolved by name and
// duplicate/trailing separators dropped. Purely lexical: no filesystem call.
//
// std::filesystem::canonical() cannot be used for this. libc++ builds it on
// realpath(), libc builds realpath() on readlink(), and a PS5 title is refused
// readlink with EPERM even for /app0 (ref/PS5_PayloadSDK
// platform/include/ps5platform/libc.h, "realpath refused to a title"). Every
// canonical() call therefore fails on the console, which used to make
// scene_location() reject every real scene (W17 finding 1).
//
// False when `path` is empty, the working directory cannot be read, or the
// result is somehow not absolute; `out` is then unspecified.
bool normal_absolute(const Path& path, Path& out);

}  // namespace splat
