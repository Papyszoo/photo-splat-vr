#include "gallery/gallery.h"

#include <algorithm>
#include <system_error>

#include "gallery/fs_util.h"

namespace splat {
namespace {

// A scene folder is a directory holding scene.psplat; any other directory
// under the root is an album.
bool has_scene(const Path& dir) {
    std::error_code error;
    return std::filesystem::is_regular_file(dir / "scene.psplat", error) && !error;
}

bool has_thumb(const Path& dir) {
    std::error_code error;
    return std::filesystem::is_regular_file(dir / "thumb.jpg", error) && !error;
}

bool hidden_name(const std::string& name) { return !name.empty() && name.front() == '.'; }

// True when the path itself is a symlink, or when nothing can say it is not
// one (refusing is the safe direction).
//
// A title may not read link targets, and on the PS5 it is refused the
// non-following status as well: symlink_status() (libc++ -> lstat) answers
// EPERM for every path on /data, measured on the console in scenefix2. So the
// type the filesystem reported for the entry in its parent is asked first: the
// directory read (readdir's d_type) carries it, and directory_entry::is_symlink()
// on that entry uses it without any lstat. symlink_status() stays the fallback
// for a parent that cannot be listed and for a file system that reports no type
// for the entry.
bool symlink_or_unreadable(const Path& path) {
    const Path parent = path.parent_path();
    const std::string name = path.filename().string();
    if (!parent.empty() && !name.empty()) {
        std::error_code listing_error;
        std::filesystem::directory_iterator it(
            parent, std::filesystem::directory_options::skip_permission_denied, listing_error);
        const std::filesystem::directory_iterator end;
        while (!listing_error && it != end) {
            if (it->path().filename().string() == name) {
                std::error_code entry_error;
                const bool link = it->is_symlink(entry_error);
                if (!entry_error) return link;
                break;  // the entry's type is unknown here: ask the status below
            }
            it.increment(listing_error);
        }
    }
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    return error || std::filesystem::is_symlink(status);
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

std::vector<Path> scene_folders(const Path& directory) {
    std::vector<Path> scenes;
    std::error_code error;
    std::filesystem::directory_iterator it(directory,
                                           std::filesystem::directory_options::skip_permission_denied,
                                           error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end) {
        std::error_code item_error;
        const Path path = it->path();
        if (it->is_directory(item_error) && !item_error && !hidden_name(path.filename().string()) &&
            has_scene(path)) {
            scenes.push_back(path);
        }
        it.increment(error);
    }
    std::sort(scenes.begin(), scenes.end(), [](const Path& a, const Path& b) {
        return a.filename().string() < b.filename().string();
    });
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

SceneMeta read_meta_or_default(const Path& folder) {
    const Result<SceneMeta> meta = read_scene_meta(scene_files(folder).meta);
    return meta.ok() ? meta.value() : SceneMeta{};
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

std::vector<AlbumEntry> list_albums(const Path& root) {
    std::vector<AlbumEntry> albums;
    if (!is_dir(root)) return albums;

    AlbumEntry loose;
    loose.dir = root;
    loose.name = kAllScenesAlbumName;
    album_stats(root, &loose.scene_count, &loose.cover);

    std::error_code error;
    std::filesystem::directory_iterator it(root,
                                          std::filesystem::directory_options::skip_permission_denied,
                                          error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end) {
        std::error_code item_error;
        const Path path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(item_error) && !item_error && !hidden_name(name) && !has_scene(path)) {
            AlbumEntry album;
            album.dir = path;
            album.name = name;
            album_stats(path, &album.scene_count, &album.cover);
            albums.push_back(std::move(album));
        }
        it.increment(error);
    }

    std::sort(albums.begin(), albums.end(),
              [](const AlbumEntry& a, const AlbumEntry& b) { return a.name < b.name; });
    albums.insert(albums.begin(), std::move(loose));
    return albums;
}

std::vector<SceneEntry> list_scenes(const Path& root, const std::string& album) {
    std::vector<SceneEntry> scenes;
    const Path directory = album_dir(root, album);
    if (!is_dir(directory)) return scenes;

    for (const Path& folder : scene_folders(directory)) {
        SceneEntry entry;
        entry.folder = folder;
        entry.name = folder.filename().string();
        entry.album = directory == root ? "" : directory.filename().string();
        entry.meta = read_meta_or_default(folder);
        entry.has_thumb = has_thumb(folder);
        scenes.push_back(std::move(entry));
    }
    return scenes;
}

Status create_album(const Path& root, const std::string& name) {
    if (!valid_album_name(name)) return Status::error("invalid album name: \"" + name + "\"");
    if (is_reserved(name)) return Status::error("album name is reserved: \"" + name + "\"");

    const Path dir = root / name;
    std::error_code error;
    if (std::filesystem::exists(dir, error) && !error) {
        return Status::error("album already exists: " + dir.string());
    }
    std::filesystem::create_directories(dir, error);
    if (error) return Status::error("cannot create " + dir.string() + ": " + error.message());
    return Status::success();
}

Status rename_album(const Path& root, const std::string& from, const std::string& to) {
    if (!valid_album_name(from)) return Status::error("invalid album name: \"" + from + "\"");
    if (!valid_album_name(to)) return Status::error("invalid album name: \"" + to + "\"");
    if (is_reserved(to)) return Status::error("album name is reserved: \"" + to + "\"");
    if (from.empty() || is_reserved(from)) return Status::error("cannot rename album \"" + from + "\"");

    const Path source = root / from;
    if (!is_dir(source) || has_scene(source)) return Status::error("not an album: " + source.string());
    if (from == to) return Status::success();

    const Path target = root / to;
    std::error_code error;
    if (std::filesystem::exists(target, error) && !error) {
        return Status::error("album already exists: " + target.string());
    }
    std::filesystem::rename(source, target, error);
    if (error) {
        return Status::error("cannot rename " + source.string() + " to " + target.string() + ": " +
                             error.message());
    }
    return Status::success();
}

Status delete_album(const Path& root, const std::string& name) {
    if (name.empty()) return Status::error("cannot delete the loose-scene album");
    if (!valid_album_name(name)) return Status::error("invalid album name: \"" + name + "\"");
    if (is_reserved(name)) return Status::error("cannot delete album \"" + name + "\"");

    const Path dir = root / name;
    if (!is_dir(dir) || has_scene(dir)) return Status::error("not an album: " + dir.string());

    std::error_code error;
    std::filesystem::directory_iterator it(dir,
                                           std::filesystem::directory_options::skip_permission_denied,
                                           error);
    const std::filesystem::directory_iterator end;
    if (!error && it != end) {
        std::error_code item_error;
        if (it->is_directory(item_error) && !item_error && has_scene(it->path())) {
            return Status::error("album holds a scene: " + dir.string());
        }
        return Status::error("album is not empty: " + dir.string());
    }

    std::filesystem::remove(dir, error);
    if (error) return Status::error("cannot delete " + dir.string() + ": " + error.message());
    return Status::success();
}

Status move_scene(const Path& root, const Path& scene_folder, const std::string& to_album) {
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

    std::error_code error;
    if (std::filesystem::exists(target, error) && !error) {
        return Status::error("scene folder already exists: " + target.string());
    }
    std::filesystem::rename(scene_folder, target, error);
    if (error) {
        return Status::error("cannot move " + scene_folder.string() + " to " + target.string() + ": " +
                             error.message());
    }
    return Status::success();
}

bool scene_location(const Path& root, const Path& scene_folder, std::string& album) {
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
    std::string album;
    if (!scene_location(root, scene_folder, album)) {
        return Status::error("not a scene folder: " + scene_folder.string());
    }
    if (!valid_album_name(new_name) || is_reserved(new_name)) {
        return Status::error("invalid scene name: \"" + new_name + "\"");
    }
    if (scene_folder.filename() == Path(new_name)) return Status::success();
    const Path target = scene_folder.parent_path() / new_name;
    std::error_code error;
    if (std::filesystem::exists(target, error) && !error) {
        return Status::error("that name already exists: " + new_name);
    }
    std::filesystem::rename(scene_folder, target, error);
    if (error) return Status::error("cannot rename " + scene_folder.string() + ": " + error.message());
    return Status::success();
}

Status delete_scene(const Path& root, const Path& scene_folder) {
    std::string album;
    if (!scene_location(root, scene_folder, album)) {
        return Status::error("not a scene folder: " + scene_folder.string());
    }
    std::error_code error;
    std::filesystem::remove_all(scene_folder, error);
    if (error) return Status::error("cannot delete " + scene_folder.string() + ": " + error.message());
    return Status::success();
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

    std::error_code error;
    if (!std::filesystem::exists(root / base, error) || error) return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + "-" + std::to_string(suffix);
        if (!std::filesystem::exists(root / candidate, error) || error) return candidate;
    }
    return base + "-1000";
}

}  // namespace splat
