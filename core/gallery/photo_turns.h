// A photo's turn as a file action, not a picker-only choice.
//
// The user's decision (2026-11-05) is Windows' behaviour: rotate left or right
// by 90 degrees on an entry, and the rotated version overrides the previous
// one. There is no copy and no second entry. For a JPEG or a baseline TIFF the
// source's Orientation tag is rewritten (no pixel re-encode, no quality loss);
// for every other format, a read-only location or a failed write the source is
// left alone and the turn is kept in <state>/photo-turns.json, keyed by
// location and path. A camera RAW file is never written to.
//
// The saved turn is exactly what a conversion still has to apply: the job's
// existing rotate_quarters (core/jobs/job_queue.h) carries it, after the EXIF
// orientation (core/image/decode.h). After a successful rewrite the saved turn
// is cleared, so the conversion applies the file's tag once and not twice. A
// rewritable file that cannot be read at all is an error, not a saved turn: the
// conversion could not read it either (a stale listing after a delete).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// <state>/photo-turns.json: one entry per file that carries a save-on-this-
// console turn. `load_all()` is one read for a whole picker listing; save()
// edits one entry and writes the file atomically.
class PhotoTurnStore {
public:
    explicit PhotoTurnStore(Path state_dir);
    const Path& state_dir() const { return state_dir_; }

    // The key the file is stored under: vfs::path_key(file), so a network path
    // keys by location id and path inside it and a local one by its lexical
    // absolute path. A missing or corrupt file is an empty map, not an error: a
    // damaged cache must not stop the picker from listing photos.
    std::map<std::string, int> load_all() const;
    // 0..3 clockwise quarters saved for `file`, 0 when none.
    int load(const Path& file) const;
    // Sets (1..3) or removes (0) the entry. A removal of an absent entry is a
    // success with no write, so clearing a rewritten file costs nothing on the
    // normal path.
    Status save(const Path& file, int quarters) const;

private:
    Path state_dir_;
};

// How a turn was applied to one file.
struct PhotoTurnResult {
    enum class Route {
        rewritten,  // the source file's Orientation tag was rewritten
        saved,      // the source file was left alone; the turn is in the store
    };
    Route route = Route::saved;
    int quarters = 0;      // saved route: the turn still on top of the file (0..3)
    int orientation = 1;   // rewritten route: the file's Orientation tag now
    bool file_changed = false;
    std::string note;      // why the rewrite was not possible (saved route)
};

// The file's current turn together with one more `delta_quarters` clockwise
// (negative = counter-clockwise). A rewritable file gets its Orientation tag
// advanced by the whole turn it currently shows (its own saved turn plus the
// delta); otherwise the saved turn advances. `allow_rewrite` false is the
// console-captures path: the helper's files are never touched.
//
// The returned quarters is what the picker must show and what the conversion
// must apply: 0 after a rewrite, the new saved turn otherwise.
Result<PhotoTurnResult> rotate_photo_file(const Path& file, int delta_quarters,
                                          const PhotoTurnStore& store, bool allow_rewrite);

}  // namespace splat
