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

// What a viewer boots from when no scene was picked by hand: one album and its
// scenes, in the gallery's own order (the same list_albums/list_scenes a screen
// reads).
struct BootAlbum {
    std::string album;               // album folder name; "" for the loose scenes
    std::vector<SceneEntry> scenes;  // that album's scenes, gallery order
    size_t index = 0;                // the scene to show first
};

// The album a fresh start shows: the loose-scene album (Unsorted) first, then
// the albums in list_albums() order - the first one that holds a scene. A
// `preferred` folder name (viewer.env's SCENE) wins wherever it sits, and its
// own album comes with it, so the caller's scene stepping stays inside one
// album exactly as the gallery screen lists it. Empty `scenes` means the
// gallery holds no scene at all (a fresh install before any seed, an empty
// data folder); that is not an error: the caller opens its menu instead.
BootAlbum boot_album(const Path& root, const std::string& preferred = "");

// One scene the boot loader walked past because it would not read.
struct BootSceneSkip {
    std::size_t index = 0;  // its place in the boot playlist
    std::string name;       // scene folder name
    std::string error;      // read_scene_path()'s message
};

// What the viewer got from its boot playlist.
struct BootSceneLoad {
    std::size_t index = 0;   // playlist index of the scene that loaded
    bool loaded = false;     // false: every scene tried failed, the empty-view path
    LoadedScene scene;
    std::vector<BootSceneSkip> skipped;  // one entry per failed scene, in order
};

// Reads the boot playlist's scene folders from `start` to its end and returns
// the first that loads, so a corrupt, truncated, unreadable or wrong-format
// scene is skipped instead of ending the session. `loaded == false` means none
// of the scenes tried would read; the caller then behaves as if the gallery
// were empty (empty view, the menu on the folders home). The caller logs one
// line per `skipped` entry.
BootSceneLoad load_boot_scene(const std::vector<Path>& scene_folders, std::size_t start = 0);

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
