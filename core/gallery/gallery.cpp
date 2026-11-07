#include "gallery/gallery.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include "gallery/fs_util.h"
#include "gallery/visibility.h"
#include "vfs/io.h"

namespace splat {
namespace {

// A scene folder is a directory holding scene.psplat; any other directory
// under the root is an album.
bool has_scene(const Path& dir) {
    vfs::FileStat status;
    return vfs::stat(scene_files(dir).splat, &status).ok() && status.regular;
}

bool has_thumb(const Path& dir) {
    vfs::FileStat status;
    return vfs::stat(scene_files(dir).thumbnail, &status).ok() && status.regular;
}

bool hidden_name(const std::string& name) { return !name.empty() && name.front() == '.'; }

// The one refusal every folder mutation on a location shares: the console can
// read a share, not rearrange it (create/rename/delete would be a second write
// path with its own conflict rules, and L3 keeps the mutations local).
Status remote_mutation_refused(const Path& root) {
    if (!vfs::is_remote(root)) return Status::success();
    return Status::error(kRemoteMutationRefusal);
}

// True when the path itself is a symlink, or when nothing can say it is not
// one (refusing is the safe direction).
//
// A title may not read link targets, and on the PS5 it is refused the
// non-following status as well: symlink_status() (libc++ -> lstat) answers
// EPERM for every path on /data, measured on the console in scenefix2. So the
// type the file system reported for the entry in its parent is asked: the
// directory read (readdir's d_type) carries it (vfs::DirEntry::symlink). A
// parent that cannot be listed, or an entry it does not show, counts as
// unreadable and is refused exactly as before.
bool symlink_or_unreadable(const Path& path) {
    const Path parent = path.parent_path();
    const std::string name = path.filename().string();
    if (parent.empty() || name.empty()) return true;
    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(parent, &entries).ok()) return true;
    for (const vfs::DirEntry& entry : entries) {
        if (entry.name == name) return entry.symlink;
    }
    return true;
}

// The folder an album key names. "" is the loose-scene album at the root;
// kAllScenesAlbumName is its display name and resolves there too, unless a
// hand-made album folder actually took that name.
Path album_dir(const Path& root, const std::string& album) {
    if (album.empty()) return root;
    if (album == kAllScenesAlbumName) {
        const Path named = root / album;
        if (is_dir(named) && !has_scene(named)) return named;
        return root;
    }
    return root / album;
}

// The scene folders among one directory listing. Split out so a caller that
// already listed the directory (list_scenes_checked) does not list it twice.
std::vector<Path> scene_folders_from(const Path& directory,
                                     const std::vector<vfs::DirEntry>& entries) {
    std::vector<Path> scenes;
    for (const vfs::DirEntry& entry : entries) {
        if (!entry.directory || entry.symlink || hidden_name(entry.name)) continue;
        const Path path = directory / entry.name;
        if (has_scene(path)) scenes.push_back(path);
    }
    std::sort(scenes.begin(), scenes.end(), [](const Path& a, const Path& b) {
        return a.filename().string() < b.filename().string();
    });
    return scenes;
}

std::vector<Path> scene_folders(const Path& directory) {
    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(directory, &entries).ok()) return {};
    return scene_folders_from(directory, entries);
}

// The scene entries of one directory listing, policy applied. list_scenes()
// and list_scenes_checked() differ only in how they obtain `entries` and how
// they report a listing that refused to run.
std::vector<SceneEntry> scene_entries_from(const Path& root, const Path& directory,
                                           const std::vector<vfs::DirEntry>& entries) {
    std::vector<SceneEntry> scenes;
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    const VisibilityKey placement = listing_placement(root, directory);
    for (const Path& folder : scene_folders_from(directory, entries)) {
        SceneEntry entry;
        entry.folder = folder;
        entry.name = folder.filename().string();
        entry.album = directory == root ? "" : directory.filename().string();
        // One meta.json read per scene: it is the entry's display data. The
        // policy asks about the placement only (stage U1), never the meta.
        const Result<SceneMeta> meta = read_scene_meta(scene_files(folder).meta);
        entry.meta_readable = meta.ok();
        entry.meta = meta.ok() ? meta.value() : SceneMeta{};
        if (visibility->enabled && !visibility->allows_scene(folder, placement)) continue;
        entry.has_thumb = has_thumb(folder);
        scenes.push_back(std::move(entry));
    }
    return scenes;
}

void album_stats(const Path& directory, size_t* scene_count, Path* cover) {
    const std::vector<Path> scenes = scene_folders(directory);
    *scene_count = scenes.size();
    cover->clear();
    for (const Path& scene : scenes) {
        if (has_thumb(scene)) {
            *cover = scene;
            return;
        }
    }
    if (!scenes.empty()) *cover = scenes.front();
}

// Reads one scene's meta.json for the caller's display fields and asks the
// policy about the placement. `meta` and `readable` are filled either way, so
// the caller can build the entry from the same read.
bool scene_visible(const Visibility& state, const VisibilityKey& placement, const Path& folder,
                   SceneMeta* meta, bool* readable) {
    const Result<SceneMeta> read = read_scene_meta(scene_files(folder).meta);
    *readable = read.ok();
    *meta = read.ok() ? read.value() : SceneMeta{};
    return state.allows_scene(folder, placement);
}

bool is_reserved(const std::string& name) { return name == kAllScenesAlbumName; }

}  // namespace

bool valid_album_name(const std::string& name) {
    if (name.empty() || name.size() > 48 || name.front() == '.') return false;
    for (const char c : name) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ';
        if (!allowed) return false;
    }
    return true;
}

std::vector<AlbumEntry> list_albums_all(const Path& root) {
    std::vector<AlbumEntry> albums;
    if (!is_dir(root)) return albums;

    AlbumEntry loose;
    loose.dir = root;
    loose.name = kAllScenesAlbumName;
    album_stats(root, &loose.scene_count, &loose.cover);

    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(root, &entries).ok()) entries.clear();
    for (const vfs::DirEntry& entry : entries) {
        if (!entry.directory || entry.symlink || hidden_name(entry.name)) continue;
        const Path path = root / entry.name;
        if (has_scene(path)) continue;
        AlbumEntry album;
        album.dir = path;
        album.name = entry.name;
        album_stats(path, &album.scene_count, &album.cover);
        albums.push_back(std::move(album));
    }

    std::sort(albums.begin(), albums.end(),
              [](const AlbumEntry& a, const AlbumEntry& b) { return a.name < b.name; });
    albums.insert(albums.begin(), std::move(loose));
    return albums;
}

std::vector<AlbumEntry> list_albums(const Path& root) {
    std::vector<AlbumEntry> albums = list_albums_all(root);
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    if (!visibility->enabled) return albums;

    std::vector<AlbumEntry> visible;
    visible.reserve(albums.size());
    for (AlbumEntry& album : albums) {
        const VisibilityKey placement = listing_placement(root, album.dir);
        if (!visibility->allows(placement)) continue;

        // The count and the cover are the visible ones: a folder whose scenes
        // are all hidden is an empty folder with a folder icon, never a tile
        // showing a hidden picture.
        std::size_t count = 0;
        Path first_visible;
        Path first_thumb;
        for (const Path& folder : scene_folders(album.dir)) {
            SceneMeta meta;
            bool readable = false;
            if (!scene_visible(*visibility, placement, folder, &meta, &readable)) continue;
            ++count;
            if (first_visible.empty()) first_visible = folder;
            if (first_thumb.empty() && has_thumb(folder)) first_thumb = folder;
        }
        album.scene_count = count;
        album.cover = first_thumb.empty() ? first_visible : first_thumb;
        visible.push_back(std::move(album));
    }
    return visible;
}

Result<std::vector<AlbumEntry>> list_albums_checked(const Path& root) {
    vfs::FileStat status;
    const Status looked = vfs::stat(root, &status);
    if (!looked.ok()) return Result<std::vector<AlbumEntry>>::failure(looked.message());
    if (!status.directory) {
        return Result<std::vector<AlbumEntry>>::failure("not a folder: " + root.string());
    }
    return Result<std::vector<AlbumEntry>>::success(list_albums(root));
}

std::vector<SceneEntry> list_scenes(const Path& root, const std::string& album) {
    const Path directory = album_dir(root, album);
    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(directory, &entries).ok()) return {};
    return scene_entries_from(root, directory, entries);
}

Result<std::vector<SceneEntry>> list_scenes_checked(const Path& root, const std::string& album) {
    const Path directory = album_dir(root, album);
    vfs::FileStat status;
    const Status looked = vfs::stat(directory, &status);
    if (!looked.ok()) return Result<std::vector<SceneEntry>>::failure(looked.message());
    if (!status.directory) {
        return Result<std::vector<SceneEntry>>::failure("not a folder: " + directory.string());
    }
    std::vector<vfs::DirEntry> entries;
    const Status listed = vfs::list(directory, &entries);
    if (!listed.ok()) return Result<std::vector<SceneEntry>>::failure(listed.message());
    return Result<std::vector<SceneEntry>>::success(
        scene_entries_from(root, directory, entries));
}

BootAlbum boot_album(const Path& root, const std::string& preferred) {
    BootAlbum first;      // the gallery's own first scene: Unsorted, then albums
    BootAlbum chosen;     // the SCENE= album, when that scene is in the gallery
    bool found_preferred = false;
    // One pass over the gallery in its own order: the implicit Unsorted album
    // is first (list_albums), then the named albums sorted by name. The first
    // album that holds a scene is the fallback; a `preferred` scene anywhere in
    // the gallery replaces it, and its own album comes with it.
    for (const AlbumEntry& album : list_albums(root)) {
        if (album.scene_count == 0) continue;
        // "" names the root's loose scenes; every other album its folder.
        const std::string key = album.dir == root ? std::string() : album.name;
        std::vector<SceneEntry> scenes = list_scenes(root, key);
        if (scenes.empty()) continue;
        if (first.scenes.empty()) {
            first.album = key;
            first.scenes = scenes;  // a copy: `scenes` is searched below
            first.index = 0;
        }
        if (preferred.empty()) break;  // no SCENE=: the gallery's first scene is the answer
        bool found = false;
        for (size_t i = 0; i < scenes.size(); ++i) {
            if (scenes[i].name != preferred) continue;
            chosen.album = key;
            chosen.scenes = std::move(scenes);
            chosen.index = i;
            found_preferred = true;
            found = true;
            break;
        }
        if (found) break;  // the SCENE= scene was found: no album is read further
    }
    return found_preferred ? chosen : first;
}

BootSceneLoad load_boot_scene(const std::vector<Path>& scene_folders, std::size_t start) {
    BootSceneLoad load;
    load.index = start;
    // A hidden scene is skipped silently: its name must not reach a log line or
    // a skipped-scene message (stage H, "A hidden folder is absent everywhere").
    const std::shared_ptr<const Visibility> visibility = current_visibility();
    for (std::size_t i = start; i < scene_folders.size(); ++i) {
        if (!visibility->allows_scene_folder(scene_folders[i])) continue;
        const Path scene_file = scene_files(scene_folders[i]).splat;
        Result<LoadedScene> result = read_scene_path(scene_file);
        if (!result.ok()) {
            load.skipped.push_back(
                BootSceneSkip{i, scene_folders[i].filename().string(), result.message()});
            continue;
        }
        load.index = i;
        load.loaded = true;
        load.scene = result.take();
        break;
    }
    return load;
}

Status create_album(const Path& root, const std::string& name) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    if (!valid_album_name(name)) return Status::error("invalid album name: \"" + name + "\"");
    if (is_reserved(name)) return Status::error("album name is reserved: \"" + name + "\"");

    const Path dir = root / name;
    vfs::FileStat status;
    if (vfs::stat(dir, &status).ok() && status.exists) {
        return Status::error("album already exists: " + dir.string());
    }
    return vfs::make_dirs(dir);
}

Status rename_album(const Path& root, const std::string& from, const std::string& to) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    if (!valid_album_name(from)) return Status::error("invalid album name: \"" + from + "\"");
    if (!valid_album_name(to)) return Status::error("invalid album name: \"" + to + "\"");
    if (is_reserved(to)) return Status::error("album name is reserved: \"" + to + "\"");
    if (from.empty() || is_reserved(from)) return Status::error("cannot rename album \"" + from + "\"");

    const Path source = root / from;
    if (!is_dir(source) || has_scene(source)) return Status::error("not an album: " + source.string());
    if (from == to) return Status::success();

    const Path target = root / to;
    vfs::FileStat status;
    if (vfs::stat(target, &status).ok() && status.exists) {
        return Status::error("album already exists: " + target.string());
    }
    // The rename goes through the VFS like every other folder write: an adapter
    // with no replace (and a test's fake file system) has to see it, and a
    // cross-file-system target must be refused by the same rule everywhere.
    return vfs::rename_file(source, target);
}

Status delete_album(const Path& root, const std::string& name) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    if (name.empty()) return Status::error("cannot delete the loose-scene album");
    if (!valid_album_name(name)) return Status::error("invalid album name: \"" + name + "\"");
    if (is_reserved(name)) return Status::error("cannot delete album \"" + name + "\"");

    const Path dir = root / name;
    if (!is_dir(dir) || has_scene(dir)) return Status::error("not an album: " + dir.string());

    // An album is removable only when it is empty: a stray file or a scene
    // folder keeps it.
    std::vector<vfs::DirEntry> entries;
    if (!vfs::list(dir, &entries).ok()) entries.clear();
    if (!entries.empty()) {
        for (const vfs::DirEntry& entry : entries) {
            if (entry.directory && !entry.symlink && has_scene(dir / entry.name)) {
                return Status::error("album holds a scene: " + dir.string());
            }
        }
        return Status::error("album is not empty: " + dir.string());
    }

    // The directory is empty: rmdir, through the C library so this file needs
    // no filesystem library (the local adapter owns that).
    if (std::remove(dir.string().c_str()) != 0) {
        return Status::error("cannot delete " + dir.string() + ": " + std::strerror(errno));
    }
    return Status::success();
}

Status move_scene(const Path& root, const Path& scene_folder, const std::string& to_album) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    if (!is_dir(scene_folder) || !has_scene(scene_folder)) {
        return Status::error("not a scene folder: " + scene_folder.string());
    }
    if (!to_album.empty()) {
        if (!valid_album_name(to_album) || is_reserved(to_album)) {
            return Status::error("invalid album name: \"" + to_album + "\"");
        }
        const Path target_album = root / to_album;
        if (!is_dir(target_album) || has_scene(target_album)) {
            return Status::error("album does not exist: " + target_album.string());
        }
    }

    const Path target = (to_album.empty() ? root : root / to_album) / scene_folder.filename();
    if (target == scene_folder) return Status::success();

    vfs::FileStat status;
    if (vfs::stat(target, &status).ok() && status.exists) {
        return Status::error("scene folder already exists: " + target.string());
    }
    return vfs::rename_file(scene_folder, target);
}

bool scene_location(const Path& root, const Path& scene_folder, std::string& album) {
    // A location path is not an OS path: normal_absolute() would prepend the
    // working directory to "smb0:/..." and compare nonsense. Nothing that
    // rearranges folders works on a location anyway (remote_mutation_refused).
    if (vfs::is_remote(root) || vfs::is_remote(scene_folder)) return false;
    Path base;
    Path folder;
    if (!normal_absolute(root, base) || !normal_absolute(scene_folder, folder)) return false;
    if (symlink_or_unreadable(folder) || !is_dir(folder) || !has_scene(folder)) return false;

    // The scene sits directly in the root or in exactly one album under it.
    // The comparison is lexical: both paths are absolute and normal above.
    const Path relative = folder.lexically_relative(base);
    std::vector<std::string> parts;
    for (const Path& part : relative) parts.push_back(part.string());
    if (parts.empty() || parts.size() > 2) return false;
    for (const std::string& part : parts) {
        if (part.empty() || part == "." || part == ".." || hidden_name(part)) return false;
    }
    if (parts.size() == 2) {
        const Path album_folder = base / parts[0];
        if (symlink_or_unreadable(album_folder) || !is_dir(album_folder) || has_scene(album_folder)) {
            return false;
        }
        album = parts[0];
    } else {
        album.clear();
    }
    return true;
}

Status rename_scene(const Path& root, const Path& scene_folder, const std::string& new_name) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    std::string album;
    if (!scene_location(root, scene_folder, album)) {
        return Status::error("not a scene folder: " + scene_folder.string());
    }
    if (!valid_album_name(new_name) || is_reserved(new_name)) {
        return Status::error("invalid scene name: \"" + new_name + "\"");
    }
    if (scene_folder.filename() == Path(new_name)) return Status::success();
    const Path target = scene_folder.parent_path() / new_name;
    vfs::FileStat status;
    if (vfs::stat(target, &status).ok() && status.exists) {
        return Status::error("that name already exists: " + new_name);
    }
    return vfs::rename_file(scene_folder, target);
}

Status delete_scene(const Path& root, const Path& scene_folder) {
    const Status remote = remote_mutation_refused(root);
    if (!remote.ok()) return remote;
    std::string album;
    if (!scene_location(root, scene_folder, album)) {
        return Status::error("not a scene folder: " + scene_folder.string());
    }
    return vfs::remove_tree(scene_folder);
}

std::string default_album_name(std::time_t time, const Path& root) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    const std::string base(buffer);

    vfs::FileStat status;
    if (!vfs::stat(root / base, &status).ok() || !status.exists) return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + "-" + std::to_string(suffix);
        if (!vfs::stat(root / candidate, &status).ok() || !status.exists) return candidate;
    }
    return base + "-1000";
}

}  // namespace splat
