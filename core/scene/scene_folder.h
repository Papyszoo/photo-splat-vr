#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "scene/psplat.h"
#include "scene/splat_scene.h"

namespace splat {

struct Intrinsics {
    uint32_t width = 0;
    uint32_t height = 0;
    float fx = 0.0f;  // focal length in original-image pixels
    float fy = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
};

// Contents of meta.json next to scene.psplat. format_version is the .psplat
// version that the writer produced, so readers can warn before parsing.
struct SceneMeta {
    uint32_t format_version = kPsplatVersion;
    std::string backend;  // e.g. "sharp", "depth-anything-v2-small"
    Intrinsics intrinsics;
    double seconds_taken = 0.0;
    uint32_t splat_count = 0;
    // Optional provenance the gallery shows; older meta.json files do not have
    // them and still read. Empty strings are not written.
    std::string captured_at;  // capture time as the source reported it, may be empty
    std::string source;       // e.g. inbox, usb0, captures; may be empty
};

std::string scene_meta_to_json(const SceneMeta& meta);
Result<SceneMeta> scene_meta_from_json(const std::string& json);
Status write_scene_meta(const Path& path, const SceneMeta& meta);
Result<SceneMeta> read_scene_meta(const Path& path);

struct SceneFiles {
    Path original;   // original.jpg
    Path thumbnail;  // thumb.jpg
    Path splat;      // scene.psplat
    Path meta;       // meta.json
};

SceneFiles scene_files(const Path& folder);

// Writes original.jpg, thumb.jpg, scene.psplat and meta.json. The JPEG buffers
// are copied verbatim; encoding happens in the ingest/decode layer. The stored
// meta.splat_count is taken from the scene.
Status write_scene_folder(const Path& folder, const SplatScene& scene, const SceneMeta& meta,
                          const std::vector<uint8_t>& original_jpeg,
                          const std::vector<uint8_t>& thumbnail_jpeg);

struct LoadedScene {
    SplatScene scene;
    SceneMeta meta;
};

// Loads scene.psplat and meta.json. The image files are not read.
Result<LoadedScene> read_scene_folder(const Path& folder);

// Loads a scene file directly: a .psplat (header, optional meta.json) or a
// .ply (SHARP export, optional intrinsic elements). This is the entry point
// the playlist preloader uses off the main thread; it does not touch Vulkan.
Result<LoadedScene> read_scene_path(const Path& path);

}  // namespace splat
