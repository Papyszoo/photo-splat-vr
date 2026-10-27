#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "scene/scene_folder.h"

namespace splat {

// Read-only album/scene listing plus the few folder mutations the gallery
// screens need. Albums are plain directories under the scene root; a scene is
// a directory that holds scene.psplat. There is no index: the filesystem is
// the only state.

// Display name of the implicit album that holds the loose scenes directly
// under the root (the scenes that are in no folder). Its folder name (the
// `album` argument of list_scenes) is the empty string. create_album/
// rename_album refuse this name, so a real folder can only end up with it by
// hand.
inline constexpr const char* kAllScenesAlbumName = "Unsorted";

struct AlbumEntry {
    Path dir;               // album directory; the root itself for the implicit album
    std::string name;       // folder name; kAllScenesAlbumName for the implicit album
    size_t scene_count = 0;
    // Folder of the first scene (by name) that has a thumb.jpg, else of the
    // first scene, else empty. Pass it to load_scene_thumb().
    Path cover;
};

struct SceneEntry {
    Path folder;
    std::string name;   // folder name
    std::string album;  // album folder name; empty for the loose-scene album
    SceneMeta meta;     // default-constructed when meta.json is missing or unreadable
    bool has_thumb = false;
};

// False for the empty string, names longer than 48, a leading dot and any
// character outside [A-Za-z0-9._ -].
bool valid_album_name(const std::string& name);

// Albums sorted by name; the implicit loose-scene album (kAllScenesAlbumName)
// always first when the root is readable. A missing root yields an empty list,
// not an error. Entries whose name starts with '.' are ignored.
std::vector<AlbumEntry> list_albums(const Path& root);

// Scenes of one album, sorted by folder name. `album` is the album folder name
// under `root`; "" is the loose-scene album, and kAllScenesAlbumName resolves
// to the root as well (unless a hand-made folder of that name exists). A
// missing album directory yields an empty list.
std::vector<SceneEntry> list_scenes(const Path& root, const std::string& album = "");

// Creates root/<name> (and the root). Fails when the name is invalid, reserved
// or already taken.
Status create_album(const Path& root, const std::string& name);

// Renames an existing album. Refuses an invalid/reserved new name and an
// existing target. The unchanged-name case is a no-op success.
Status rename_album(const Path& root, const std::string& from, const std::string& to);

// Removes an album only when it holds no scene and is empty. The root album
// ("") cannot be deleted.
Status delete_album(const Path& root, const std::string& name);

// Moves a scene folder into root/<to_album> ("" = loose scenes), keeping its
// folder name. Refuses a missing scene, a missing target album and an existing
// target folder. Moving into the album it is already in is a no-op success.
Status move_scene(const Path& root, const Path& scene_folder, const std::string& to_album);

// Where a scene folder sits under `root`: `album` becomes "" for a loose
// scene, the album folder name otherwise. False for anything that is not a
// scene directly in the root or in one album under it: a path outside the
// root, the root itself, deeper nesting, a symlink, a hidden name, or a folder
// without scene.psplat. rename_scene/delete_scene only act on such folders.
// The paths are made absolute and compared lexically (trailing separators and
// "." resolved); canonical() is never called, because a PS5 title is refused
// realpath/readlink. A symlink is found from the type of its entry in the
// parent's directory read, because the non-following status (lstat) is refused
// on the console too.
bool scene_location(const Path& root, const Path& scene_folder, std::string& album);

// Renames a scene folder in place (same album). The new name follows the
// album-name rules (valid_album_name); an existing target is refused. The
// unchanged name is a no-op success.
Status rename_scene(const Path& root, const Path& scene_folder, const std::string& new_name);

// Deletes one scene folder (and only that) after scene_location() accepted it.
Status delete_scene(const Path& root, const Path& scene_folder);

// "YYYY-MM-DD" in local time, then "-2", "-3", ... until it does not exist
// under root.
std::string default_album_name(std::time_t time, const Path& root);

}  // namespace splat
