// Thinning an imported scene to the renderer's splat budget (SPEC §3.7).
//
// The kept splats are the ones that contribute most to the image: a Gaussian's
// volume (its three scales multiplied) times how opaque it is. The selection is
// an nth_element over indices and the survivors keep their original order, so
// two runs on the same file produce byte-identical scenes and the depth-sorted
// order an exporter chose survives where it can.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "scene/splat_scene.h"
#include "splat_import/splat_import.h"

namespace splat {

// Keeps at most `limit` splats, in their original index order. A scene that
// already fits, or a `limit` of 0 (meaning "no limit"), is left untouched.
// `report` (when given) gets source_count/kept_count/thinned and the note
// "thinned 4,000,000 to 2,000,000 splats" when anything was dropped.
void thin_scene(SplatScene& scene, uint32_t limit, ImportReport* report);

}  // namespace splat
