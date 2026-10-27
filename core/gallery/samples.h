// The sample scenes a release package carries, and the one-time seed into the
// gallery (W31).
//
// A v0.1 package ships a few ready-made scenes under samples/ next to
// eboot.bin (/app0/samples inside the title), so a fresh install has something
// to show before the user converts a photo. The first start copies them into
// the "Samples" album under the scene root and then writes a marker file under
// the data dir: a user who deletes the album does not get it back on the next
// start.
//
// The copy reuses core/gallery: create_album() owns the album-name rules and
// list_scenes() owns what counts as a scene folder, so a bundled folder that is
// not a scene is simply not copied. A folder that already exists in the album
// is left alone, which keeps the seed idempotent and makes a name collision
// cost one sample rather than overwrite user data.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

#include "common/path.h"

namespace splat {

// The album the bundled scenes land in; a valid album name (gallery.h).
inline constexpr const char* kSamplesAlbumName = "Samples";

struct SamplesSeed {
    // The marker was already there: nothing was examined or copied.
    bool already_seeded = false;
    // Scene folders copied by this call.
    size_t scenes = 0;
    // Non-empty when the seed stopped early. The marker stays absent then, so
    // the next start tries again (a partially copied scene is removed).
    std::string error;

    bool ok() const { return error.empty(); }
};

// Copies every scene folder in `bundle` into `<root>/Samples`, unless `marker`
// exists or `bundle` is not a directory (no samples in this package: no marker,
// nothing created; the build still passes). The marker's parent directories are
// created as needed. Never throws.
SamplesSeed seed_samples(const Path& bundle, const Path& root, const Path& marker);

}  // namespace splat
