#pragma once

#include <cmath>
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

    // The visibility key of the folder the scene came from (stage H):
    // "folder:/data/Photos", "source:inbox", "location:smb0", "album:Family A",
    // ... (core/gallery/visibility.h). Written by the job at conversion/import
    // time and carried in the queue, so a hidden folder hides the scene it
    // produced wherever the scene file lives. Empty for scenes written before
    // the feature and for a caller that has no policy context; the reader then
    // falls back to the album the scene sits in (the legacy rule).
    std::string source_folder;

    // Where an imported scene came from: the source format token ("splat",
    // "spz", ...) and the splat count that file declared before thinning.
    // Empty/0 for a converted scene, and both are optional in meta.json.
    std::string origin_format;
    uint32_t origin_count = 0;
    // True when meta.json carried the "origin" object at all: the imported-scene
    // marker, and the one rule scene_is_imported() (scene/scene_view.h) applies
    // to the same file. It does not depend on "format" being non-empty, so a
    // writer that fills only "count" is still an import to every reader
    // (scene_meta_json_is_imported() is the shared test).
    bool origin_present = false;
};

// A quaternion that is the identity rotation: (0, 0, 0, +-1). Kept for the
// splat import readers' tests; a meta.json no longer carries a scene rotation
// (DECISIONS.md, 2026-11-05).

std::string scene_meta_to_json(const SceneMeta& meta);
Result<SceneMeta> scene_meta_from_json(const std::string& json);
// True when this meta.json text carries the imported-scene marker: an "origin"
// object (whatever it holds). A conversion writes none, so a missing marker
// means converted. This is the single test of that rule: read_scene_meta()'s
// origin_present and SceneViewStore's scene_is_imported() both answer it, so
// the panel and the viewer cannot disagree about one scene.
bool scene_meta_json_is_imported(const std::string& json);
Status write_scene_meta(const Path& path, const SceneMeta& meta);
Result<SceneMeta> read_scene_meta(const Path& path);

struct SceneFiles {
    Path original;   // original.jpg
    Path thumbnail;  // thumb.jpg
    Path splat;      // scene.psplat
    Path meta;       // meta.json
};

SceneFiles scene_files(const Path& folder);

// Writes `bytes` to `path` (truncating it), used by the scene writers for
// thumb.jpg and original.jpg. Fails when the file cannot be opened or written.
Status write_bytes_file(const Path& path, const std::vector<uint8_t>& bytes);

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
