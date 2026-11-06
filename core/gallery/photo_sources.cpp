#include "gallery/photo_sources.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "splat_import/splat_format.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr size_t kMaxPhotos = 500;
constexpr int kMaxDepth = 3;

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// "usb0" -> 0, "usb12" -> 12 for prefix "usb"; -1 for anything else.
int numbered_index(const std::string& name, const std::string& prefix) {
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return -1;
    int value = 0;
    for (size_t i = prefix.size(); i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isdigit(c)) return -1;
        value = value * 10 + (c - '0');
        if (value > 99) return -1;
    }
    return value;
}

// One level of a directory; an unreadable directory is an empty listing, which
// is what every caller here treats as "nothing to show".
std::vector<vfs::DirEntry> entries_of(const Path& directory) {
    std::vector<vfs::DirEntry> entries;
    (void)vfs::list(directory, &entries);
    return entries;
}

bool listable(const Path& directory) {
    std::vector<vfs::DirEntry> entries;
    return vfs::list(directory, &entries).ok();
}

struct Numbered {
    int index;
    Path dir;
};

// The listable `<prefix>N` directories under mnt, ascending by N.
std::vector<Numbered> numbered_dirs(const Path& mnt, const std::string& prefix) {
    std::vector<Numbered> found;
    for (const vfs::DirEntry& entry : entries_of(mnt)) {
        if (!entry.directory || entry.symlink) continue;
        const int index = numbered_index(entry.name, prefix);
        if (index < 0) continue;
        const Path path = mnt / entry.name;
        if (listable(path)) found.push_back({index, path});
    }
    std::sort(found.begin(), found.end(),
              [](const Numbered& a, const Numbered& b) { return a.index < b.index; });
    return found;
}

void add_file_entry(const Path& path, const vfs::DirEntry& entry, bool splat,
                    std::vector<PhotoEntry>* out) {
    PhotoEntry photo;
    photo.path = path;
    photo.name = entry.name;
    photo.size = entry.size;
    photo.mtime = entry.mtime;
    photo.splat = splat;
    out->push_back(std::move(photo));
}

void walk_photos(const Path& directory, int level, bool include_splats,
                 std::vector<PhotoEntry>* out);

// Files at most three levels below `source`: the recursion rule of the old
// recursive scan, rebuilt on the VFS (a location cannot be walked by the C++
// filesystem library at all). A directory at level 3 is listed but not entered,
// so a file at level 3 is the deepest one collected.
void collect_photos(const Path& directory, const std::vector<vfs::DirEntry>& entries, int level,
                    bool include_splats, std::vector<PhotoEntry>* out) {
    for (const vfs::DirEntry& entry : entries) {
        const Path path = directory / entry.name;
        if (entry.symlink) continue;
        // A dot name is not a photo. macOS File Sharing (the user's own network
        // drive) writes ".DS_Store" and one "._<name>" AppleDouble per file into
        // every folder it serves, and "._IMG_0001.jpg" ends in .jpg: shown as a
        // tile, it would be the first entry in a folder and could never open.
        // list_folder() (the console browser) already skips them.
        if (entry.name.empty() || entry.name[0] == '.') continue;
        if (entry.directory) {
            if (level < kMaxDepth) walk_photos(path, level + 1, include_splats, out);
            continue;
        }
        if (is_photo_file(path)) {
            add_file_entry(path, entry, false, out);
        } else if (include_splats && is_splat_file(path)) {
            add_file_entry(path, entry, true, out);
        }
    }
}

// The unchecked walk used for sub-directories: a folder that refuses to list
// is skipped, which is what list_photos() has always promised.
void walk_photos(const Path& directory, int level, bool include_splats,
                 std::vector<PhotoEntry>* out) {
    if (out->size() >= kMaxPhotos * 4) return;  // runaway guard only; sorted below
    collect_photos(directory, entries_of(directory), level, include_splats, out);
}

void walk_splats(const Path& directory, int level, int max_depth, std::size_t max_files,
                 std::vector<Path>* out) {
    if (out->size() >= max_files) return;
    for (const vfs::DirEntry& entry : entries_of(directory)) {
        if (out->size() >= max_files) return;
        if (entry.symlink) continue;
        if (entry.name.empty() || entry.name[0] == '.') continue;  // .DS_Store, ._*

        const Path path = directory / entry.name;
        if (entry.directory) {
            if (level < max_depth) walk_splats(path, level + 1, max_depth, max_files, out);
        } else if (is_splat_file(path)) {
            out->push_back(path);
        }
    }
}

}  // namespace

std::vector<PhotoSource> discover_sources(const SourceRoots& roots) {
    std::vector<PhotoSource> sources;
    sources.push_back(PhotoSource{"inbox", "Inbox", roots.inbox, false});

    for (const Numbered& stick : numbered_dirs(roots.mnt, "usb")) {
        const std::string number = std::to_string(stick.index);
        sources.push_back(PhotoSource{"usb" + number, "USB " + number, stick.dir, true});
    }
    return sources;
}

Result<std::vector<PhotoEntry>> list_photos_checked(const PhotoSource& source,
                                                    bool include_splats) {
    vfs::FileStat status;
    const Status looked = vfs::stat(source.dir, &status);
    if (!looked.ok()) return Result<std::vector<PhotoEntry>>::failure(looked.message());
    if (!status.directory) {
        return Result<std::vector<PhotoEntry>>::failure("not a folder: " + source.dir.string());
    }
    // The top level is listed here, not through the silent walk: an offline or
    // wrong share must reach the screen as its error, not as "no photos".
    std::vector<vfs::DirEntry> entries;
    const Status listed = vfs::list(source.dir, &entries);
    if (!listed.ok()) return Result<std::vector<PhotoEntry>>::failure(listed.message());

    std::vector<PhotoEntry> photos;
    collect_photos(source.dir, entries, 0, include_splats, &photos);

    std::sort(photos.begin(), photos.end(), [](const PhotoEntry& a, const PhotoEntry& b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.path.string() < b.path.string();
    });
    if (photos.size() > kMaxPhotos) photos.resize(kMaxPhotos);
    return Result<std::vector<PhotoEntry>>::success(std::move(photos));
}

std::vector<PhotoEntry> list_photos(const PhotoSource& source, bool include_splats) {
    Result<std::vector<PhotoEntry>> photos = list_photos_checked(source, include_splats);
    if (!photos.ok()) return {};
    return std::move(photos.value());
}

std::vector<PhotoSource> console_roots(const SourceRoots& roots) {
    std::vector<PhotoSource> out;
    vfs::FileStat status;
    if (!roots.data.empty() && vfs::stat(roots.data, &status).ok() && status.directory &&
        listable(roots.data)) {
        out.push_back(PhotoSource{"data", roots.data.string(), roots.data, false});
    }
    for (const Numbered& ext : numbered_dirs(roots.mnt, "ext")) {
        out.push_back(
            PhotoSource{"ext" + std::to_string(ext.index), ext.dir.string(), ext.dir, true});
    }
    return out;
}

std::vector<FolderEntry> list_folder(const Path& dir) {
    std::vector<FolderEntry> folders;
    std::vector<FolderEntry> photos;
    std::vector<FolderEntry> splats;
    const auto listed = [&] {
        return folders.size() + photos.size() + splats.size() < kMaxPhotos;
    };
    for (const vfs::DirEntry& item : entries_of(dir)) {
        if (!listed()) break;
        if (item.name.empty() || item.name[0] == '.') continue;
        FolderEntry entry;
        entry.path = dir / item.name;
        entry.name = item.name;
        if (item.directory && !item.symlink) {
            entry.folder = true;
            folders.push_back(std::move(entry));
        } else if (!item.directory && !item.symlink) {
            const bool photo = is_photo_file(entry.path);
            const bool splat = !photo && is_splat_file(entry.path);
            if (photo || splat) {
                entry.size = item.size;
                entry.splat = splat;
                (splat ? splats : photos).push_back(std::move(entry));
            }
        }
    }
    const auto by_name = [](const FolderEntry& a, const FolderEntry& b) {
        const std::string left = lowercase(a.name);
        const std::string right = lowercase(b.name);
        return left != right ? left < right : a.name < b.name;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(photos.begin(), photos.end(), by_name);
    std::sort(splats.begin(), splats.end(), by_name);
    std::vector<FolderEntry> out;
    out.reserve(folders.size() + photos.size() + splats.size());
    for (std::vector<FolderEntry>* group : {&folders, &photos, &splats}) {
        out.insert(out.end(), std::make_move_iterator(group->begin()),
                   std::make_move_iterator(group->end()));
    }
    return out;
}

std::vector<Path> list_splat_files(const Path& dir, int max_depth, std::size_t max_files) {
    std::vector<Path> splats;
    vfs::FileStat status;
    if (!vfs::stat(dir, &status).ok() || !status.directory) return splats;

    walk_splats(dir, 0, max_depth, max_files, &splats);
    std::sort(splats.begin(), splats.end(),
              [](const Path& a, const Path& b) { return a.string() < b.string(); });
    if (splats.size() > max_files) splats.resize(max_files);
    return splats;
}

}  // namespace splat
