// One scene's stored view, and the store that keeps it: the scale an imported
// scene carries. The scene's transform is display = scale * p_scene + offset
// (turning, moving and scaling in the viewer are the session's). The viewer
// has no rotation to change or save (DECISIONS.md, "Change 2026-11-05"); a
// "view" a Track R build left in meta.json is ignored on load and removed by
// the next save, which keeps the scale and every other key.
//
// Where it lives: the scene folder's own meta.json, so the view travels with
// the scene,
//
//     "view": {"scale": s}
//
// written only when the scale is not the default 1.0. Every meta.json written
// before this key existed therefore still reads, and a scene nobody scaled
// keeps a byte-identical file. A folder that refuses the write - a read-only
// network share - falls back to <state_dir>/views.json, keyed by the location
// id plus the path inside it (vfs::path_key). load() reads that entry after the
// scene's own meta.json: it only exists while the folder refused the last
// write, so it is the newer of the two.
//
// The I/O is the file-system interface (core/vfs): a scene on an SMB or NFS
// library is read and written through its adapter, so a saved scale on a
// network scene reaches the share and comes back on the next open, and a
// read-only share is a Status, never a crash on an OS path that does not name a
// file.
//
// meta.json is edited as JSON, never rebuilt from SceneMeta: every key this
// build does not know (a later stage's, a user's, an import's "origin")
// survives a save.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/path.h"
#include "common/status.h"

namespace splat {

struct SceneView {
    // The scene's own viewing scale, 1.0 when it has none. Only an imported
    // scene stores one: an import has no capture camera, so its natural size is
    // whatever the source tool chose, and the scale the user settles on is part
    // of that scene. A converted scene keeps Track R's rule instead - a scene
    // switch keeps the session's scale and offset (meta.json has no "scale").
    float scale = 1.0f;
};

class SceneViewStore {
public:
    // `<dataDir>/state` on the console and the desktop: views.json lives here.
    explicit SceneViewStore(Path state_dir);
    const Path& state_dir() const { return state_dir_; }

    // The scene folder's meta.json, then the console-local fallback. A scene
    // with neither - every scene written before the scale store - reads 1.0. A
    // meta.json that cannot be read, or that exists but cannot be parsed, is an
    // error, not silently 1.0: the caller logs it and shows the scene at the
    // session's scale.
    //
    // `imported`, when not null, answers scene_is_imported() from the same parse
    // of meta.json (false when it cannot be read or parsed, or is absent), so a
    // caller that needs both pays one read of a network scene, not two. It is
    // the scene folder's own answer, not the cache's.
    Result<SceneView> load(const Path& scene_folder, bool* imported = nullptr) const;

    // Writes the scale into the scene folder's meta.json, replacing only the
    // "scale" member of "view" and dropping a legacy "rotation". `local_only`,
    // when not null, is set to true when meta.json could not be written and the
    // scale went to <state_dir>/views.json instead. The returned Status is an
    // error when the scene's own meta.json cannot be read or parsed and when
    // neither write worked.
    Status save(const Path& scene_folder, const SceneView& view,
                bool* local_only = nullptr) const;

    // True when save() persists this scene's scale: its meta.json carries an
    // "origin" object, which only an imported scene does (it has no capture
    // camera, so its natural size is the source tool's). False for a converted
    // scene, a missing folder and an unreadable meta.json; the viewer then keeps
    // the session's scale. A caller that is already loading the scene should
    // take load()'s `imported`, which is this same answer from the read that
    // already happened.
    bool persists_scale(const Path& scene_folder) const;

private:
    Path state_dir_;
};

// True when the scene folder is an imported splat: its meta.json carries the
// "origin" object imported_scene.cpp writes. A conversion writes no "origin",
// so a missing marker means converted - there is no second list to keep in
// step (DECISIONS.md, 2026-11-05). A folder whose meta.json cannot be read is
// not imported.
bool scene_is_imported(const Path& scene_folder);

// The fallback cache's key for a scene folder: vfs::path_key (the location id
// plus the path inside it for a network scene, the lexical absolute path for a
// local one), so two shares with the same folder tree do not share an entry.
Path scene_view_key(const Path& scene_folder);

// True when `view` carries a scale a viewer should apply on open: a stored,
// finite, positive value that is not 1.0.
bool scene_view_has_scale(const SceneView& view);

}  // namespace splat
