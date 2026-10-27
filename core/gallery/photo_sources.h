#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"

namespace splat {

// Where the gallery screens look for photos. `inbox` is the title's own inbox
// (LAN uploads and captures); `mnt` is the console's mount table, where
// ShadowMountPlus exposes USB sticks as usbN directories.
struct SourceRoots {
    Path inbox;
    Path mnt = "/mnt";
    // The console's data root (/data), the first place the folder browser
    // offers; empty = not offered.
    Path data;
};

struct PhotoSource {
    std::string id;     // "inbox", "usb0", ...
    std::string label;  // "Inbox", "USB 0", ...
    Path dir;
    bool removable = false;
};

struct PhotoEntry {
    Path path;
    std::string name;   // file name without the directories
    uint64_t size = 0;  // bytes; 0 when it cannot be stat'ed
    int64_t mtime = 0;  // Unix seconds; 0 when it cannot be stat'ed
};

// The inbox first, always, even when it does not exist yet; then one source per
// usbN directory under roots.mnt that opens for listing, ascending by N.
std::vector<PhotoSource> discover_sources(const SourceRoots& roots);

// Files under source.dir with a .jpg/.jpeg/.png extension (case-insensitive),
// at most three directory levels below the source, newest mtime first and at
// most 500 entries. Unreadable directories are skipped silently; a missing
// source directory yields an empty list.
std::vector<PhotoEntry> list_photos(const PhotoSource& source);

// True for a .jpg, .jpeg or .png path, case-insensitive.
bool is_photo_file(const Path& path);

// Where the console folder browser starts (photos copied over FTP can be
// anywhere the title can read): roots.data when it is a listable directory,
// then every listable roots.mnt/extN (extended storage), ascending by N. The
// label is the path itself.
std::vector<PhotoSource> console_roots(const SourceRoots& roots);

// One entry of a folder listing for the browser.
struct FolderEntry {
    Path path;
    std::string name;
    bool folder = false;  // a sub-folder; otherwise a photo file
    uint64_t size = 0;    // bytes, photos only
};

// One level of `dir`: its sub-folders (names starting with '.' skipped), then
// its photo files (is_photo_file), each group sorted by name ignoring case, at
// most 500 entries in all. A missing or unreadable folder yields an empty list.
std::vector<FolderEntry> list_folder(const Path& dir);

}  // namespace splat
