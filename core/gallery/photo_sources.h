#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "image/format.h"

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

// The inbox first, always, even when it does not exist yet; then one source per
// usbN directory under roots.mnt that opens for listing, ascending by N.
std::vector<PhotoSource> discover_sources(const SourceRoots& roots);

// Where the console folder browser starts (photos copied over FTP can be
// anywhere the title can read): roots.data when it is a listable directory,
// then every listable roots.mnt/extN (extended storage), ascending by N. The
// label is the path itself.
std::vector<PhotoSource> console_roots(const SourceRoots& roots);

// One entry of a folder listing for the browser.
struct FolderEntry {
    Path path;
    std::string name;
    bool folder = false;  // a sub-folder
    bool splat = false;   // a splat file to import; otherwise a photo file
    uint64_t size = 0;    // bytes, files only
    // The splat count the file declares before thinning; 0 for a folder, a
    // photo, and a splat file (the listing does not open files, so this stays 0
    // until an import has read it).
    uint32_t source_count = 0;
};

// The one listing behind every tab of the photo picker (Inbox, Console, USB
// and the network locations). One level of `dir`: its sub-folders (names
// starting with '.' skipped), then its photo files (is_photo_file), then its
// splat files (is_splat_file), each group sorted by name ignoring case, at most
// 500 entries in all. A missing or unreadable folder yields an empty list.
std::vector<FolderEntry> list_folder(const Path& dir);

// The same one level, but a folder that cannot be read is an error carrying the
// file system's own message (a network share that is offline, a wrong path),
// where list_folder() answers an empty list. Local and network paths take this
// one route: a network location is browsed folder by folder, one listing per
// visit, never walked.
Result<std::vector<FolderEntry>> list_folder_checked(const Path& dir);

}  // namespace splat
