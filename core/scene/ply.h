#pragma once

#include "common/path.h"
#include "common/status.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

namespace splat {

// Imports a binary little-endian 3DGS .ply (SH degree 0). Extra vertex
// properties such as f_rest_* are ignored, as are extra elements (intrinsic,
// extrinsic, ...).
Status import_ply(const Path& path, SplatScene& scene);

// Writes the 14 standard vertex properties in SHARP's order:
// x y z f_dc_0..2 opacity scale_0..2 rot_0..3 (all float32, little-endian).
Status export_ply(const Path& path, const SplatScene& scene);

// Reads the capture intrinsics SHARP appends after the vertex element:
// `intrinsic` (3x3 float, row-major: fx 0 cx / 0 fy cy / 0 0 1) and
// `image_size` (2 uint: width, height). Returns all-zero Intrinsics when those
// elements are absent, which is the case for export_ply output.
Result<Intrinsics> read_ply_intrinsics(const Path& path);

}  // namespace splat
