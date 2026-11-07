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

Result<std::vector<FolderEntry>> list_folder_checked(const Path& dir) {
    std::vector<vfs::DirEntry> items;
    const Status read = vfs::list(dir, &items);
    if (!read.ok()) return Result<std::vector<FolderEntry>>::failure(read.message());

    std::vector<FolderEntry> folders;
    std::vector<FolderEntry> photos;
    std::vector<FolderEntry> splats;
    const auto listed = [&] {
        return folders.size() + photos.size() + splats.size() < kMaxPhotos;
    };
    for (const vfs::DirEntry& item : items) {
        if (!listed()) break;
        // A dot name is never shown. macOS File Sharing (the user's own network
        // drive) writes ".DS_Store" and one "._<name>" AppleDouble per file into
        // every folder it serves, and "._IMG_0001.jpg" ends in .jpg: shown as a
        // tile, it would be the first entry in a folder and could never open.
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
    return Result<std::vector<FolderEntry>>::success(std::move(out));
}

std::vector<FolderEntry> list_folder(const Path& dir) {
    Result<std::vector<FolderEntry>> folder = list_folder_checked(dir);
    if (!folder.ok()) return {};
    return std::move(folder.value());
}

}  // namespace splat
