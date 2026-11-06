// Turning a scene about its viewing axis, as a file action.
//
// The user's decision (2026-11-05) is the same control as a photo: rotate left
// or right by 90 degrees on a scene entry, and the rotated version replaces the
// previous one. A scene is turned about the capture camera's forward axis (+z,
// the viewing axis), the same direction a photo turns on screen. Two presses
// fix an upside-down scene.
//
// The turn is baked into the scene's own files. scene.psplat gets the rotated
// positions and Gaussian orientations (the covariance R S S^T R^T composes as
// (Rz R) S S^T (Rz R)^T, so log_scale is unchanged); original.jpg and thumb.jpg
// are turned; meta.json's intrinsics are mapped with the same pixel transform
// as the image, and a legacy "view"."rotation" is dropped while "view"."scale"
// and every unknown key survive. A scene then matches one converted from the
// turned photo, comfort fade included: the fade uses the same intrinsics and
// capture-axis geometry as a conversion.
//
// .psplat stores fp16, so the file round trip is lossy: every turn quantizes.
// The in-memory turn itself is exact for 90-degree multiples; the drift after
// repeated file turns is measured by tests and reported.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "common/path.h"
#include "common/status.h"
#include "scene/scene_folder.h"
#include "scene/splat_scene.h"

namespace splat {

// Turns every Gaussian by `quarters` clockwise 90-degree steps about the
// scene's viewing axis. Positions map (x, y, z) -> (-y, x, z) for one step;
// orientations q (w, x, y, z) become qz * q; log_scale, sh_dc and opacity are
// untouched. The format has no higher-order SH terms and no per-splat index
// order (the renderer sorts every frame), so neither needs a fix-up.
void rotate_scene_quarters(SplatScene& scene, int quarters);

// The intrinsics after the same turn: the principal point moves with the
// pixel map and fx/fy swap when the map swaps the axes. Zero dimensions stay
// zero (an imported scene has no capture camera).
Intrinsics rotate_scene_intrinsics(const Intrinsics& intrinsics, int quarters);

// Reads the scene folder, turns the splat data, original.jpg (when the folder
// has one), thumb.jpg and meta.json's intrinsics, drops a legacy rotation, and
// replaces the files through the VFS. All new contents are written to
// ".rotate-tmp" files first; only then are they renamed over their targets in
// the order scene.psplat, original.jpg, thumb.jpg, meta.json. An interruption
// leaves every file either old or new (never half), and the scene still opens
// because the splat count is unchanged; meta.json is the commit point, so a
// window exists where the geometry is new and the intrinsics are old (and a
// second one where the images are new and the intrinsics are old). The report
// names them.
Status rotate_scene_folder(const Path& folder, int quarters, int jpeg_quality = 90);

}  // namespace splat
