// Writing an imported scene folder.
//
// A converted scene folder is original.jpg + thumb.jpg + scene.psplat +
// meta.json. An imported one has no photo, so it carries scene.psplat,
// thumb.jpg (a deterministic gradient in the scene's own mean colour, so the
// gallery tile is not an empty plate) and meta.json, and no original.jpg. The
// gallery and read_scene_folder never read original.jpg, so the folder is a
// normal scene everywhere else.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

namespace splat {

// Writes scene.psplat, thumb.jpg and meta.json into `folder` (created when
// needed). meta.splat_count is stored from the scene. A failed write leaves the
// folder without meta.json, which is what tells every reader it is incomplete.
Status write_imported_scene(const Path& folder, const SplatScene& scene, const SceneMeta& meta,
                            const std::vector<uint8_t>& thumb_jpeg);

// A width x height JPEG: a vertical gradient of the scene's mean sh_dc colour,
// scaled 0.94x at the top to 1.06x at the bottom so the mean stays the scene's
// colour. Empty for an empty scene or a non-positive size, and when encoding
// fails.
std::vector<uint8_t> make_import_thumb(const SplatScene& scene, int width, int height);

}  // namespace splat
