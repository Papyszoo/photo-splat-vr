#include "gallery/photo_sources.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <system_error>

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

// std::filesystem::last_write_time has its own clock in C++17; anchor it once
// to the system clock so PhotoEntry::mtime is a Unix timestamp.
int64_t file_mtime_seconds(const Path& path) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) return 0;
    static const auto offset =
        std::chrono::system_clock::now().time_since_epoch() -
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::filesystem::file_time_type::clock::now().time_since_epoch());
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(written.time_since_epoch()) +
        offset;
    return std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
}

bool listable(const Path& directory) {
    std::error_code error;
    const std::filesystem::directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    return !error;
}

struct Numbered {
    int index;
    Path dir;
};

// The listable `<prefix>N` directories under mnt, ascending by N.
std::vector<Numbered> numbered_dirs(const Path& mnt, const std::string& prefix) {
    std::vector<Numbered> found;
    std::error_code error;
    std::filesystem::directory_iterator it(
        mnt, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end) {
        std::error_code item_error;
        const Path path = it->path();
        const int index = numbered_index(path.filename().string(), prefix);
        if (index >= 0 && it->is_directory(item_error) && !item_error && listable(path)) {
            found.push_back({index, path});
        }
        it.increment(error);
    }
    std::sort(found.begin(), found.end(),
              [](const Numbered& a, const Numbered& b) { return a.index < b.index; });
    return found;
}

}  // namespace

bool is_photo_file(const Path& path) {
    const std::string extension = lowercase(path.extension().string());
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png";
}

std::vector<PhotoSource> discover_sources(const SourceRoots& roots) {
    std::vector<PhotoSource> sources;
    sources.push_back(PhotoSource{"inbox", "Inbox", roots.inbox, false});

    for (const Numbered& stick : numbered_dirs(roots.mnt, "usb")) {
        const std::string number = std::to_string(stick.index);
        sources.push_back(PhotoSource{"usb" + number, "USB " + number, stick.dir, true});
    }
    return sources;
}

std::vector<PhotoEntry> list_photos(const PhotoSource& source) {
    std::vector<PhotoEntry> photos;
    std::error_code error;
    if (!std::filesystem::is_directory(source.dir, error) || error) return photos;

    std::filesystem::recursive_directory_iterator it(
        source.dir, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
        std::error_code item_error;
        const Path path = it->path();
        if (it->is_directory(item_error)) {
            // Files up to three levels below the source are listed; do not
            // descend into a directory that would put them deeper.
            if (!item_error && it.depth() >= kMaxDepth) it.disable_recursion_pending();
        } else if (!item_error && it->is_regular_file(item_error) && is_photo_file(path)) {
            PhotoEntry entry;
            entry.path = path;
            entry.name = path.filename().string();
            std::error_code size_error;
            const uintmax_t size = std::filesystem::file_size(path, size_error);
            entry.size = size_error ? 0 : static_cast<uint64_t>(size);
            entry.mtime = file_mtime_seconds(path);
            photos.push_back(std::move(entry));
        }
        it.increment(error);
    }

    std::sort(photos.begin(), photos.end(), [](const PhotoEntry& a, const PhotoEntry& b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.path.string() < b.path.string();
    });
    if (photos.size() > kMaxPhotos) photos.resize(kMaxPhotos);
    return photos;
}

std::vector<PhotoSource> console_roots(const SourceRoots& roots) {
    std::vector<PhotoSource> out;
    std::error_code error;
    if (!roots.data.empty() && std::filesystem::is_directory(roots.data, error) && !error &&
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
    std::error_code error;
    std::filesystem::directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::directory_iterator end;
    while (!error && it != end && folders.size() + photos.size() < kMaxPhotos) {
        std::error_code item_error;
        const Path path = it->path();
        FolderEntry entry;
        entry.path = path;
        entry.name = path.filename().string();
        if (!entry.name.empty() && entry.name[0] != '.') {
            if (it->is_directory(item_error) && !item_error) {
                entry.folder = true;
                folders.push_back(std::move(entry));
            } else if (!item_error && it->is_regular_file(item_error) && !item_error &&
                       is_photo_file(path)) {
                std::error_code size_error;
                const uintmax_t size = std::filesystem::file_size(path, size_error);
                entry.size = size_error ? 0 : static_cast<uint64_t>(size);
                photos.push_back(std::move(entry));
            }
        }
        it.increment(error);
    }
    const auto by_name = [](const FolderEntry& a, const FolderEntry& b) {
        const std::string left = lowercase(a.name);
        const std::string right = lowercase(b.name);
        return left != right ? left < right : a.name < b.name;
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(photos.begin(), photos.end(), by_name);
    folders.insert(folders.end(), std::make_move_iterator(photos.begin()),
                   std::make_move_iterator(photos.end()));
    return folders;
}

}  // namespace splat
