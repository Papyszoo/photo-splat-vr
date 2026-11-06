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

struct PhotoEntry {
    Path path;
    std::string name;   // file name without the directories
    uint64_t size = 0;  // bytes; 0 when it cannot be stat'ed
    int64_t mtime = 0;  // Unix seconds; 0 when it cannot be stat'ed
    // A splat file listed because the caller asked for them: it is imported,
    // not converted (L2/L3).
    bool splat = false;
};

// The inbox first, always, even when it does not exist yet; then one source per
// usbN directory under roots.mnt that opens for listing, ascending by N.
std::vector<PhotoSource> discover_sources(const SourceRoots& roots);

// Files under source.dir that is_photo_file() accepts (core/image/format.h),
// at most three directory levels below the source, newest mtime first and at
// most 500 entries. Unreadable directories are skipped silently; a missing
// source directory yields an empty list. `include_splats` also lists the
// splat files is_splat_file() accepts, flagged PhotoEntry::splat; the default
// keeps the photo-only listing every existing caller expects.
std::vector<PhotoEntry> list_photos(const PhotoSource& source, bool include_splats = false);

// The same list, but a source that cannot be read is an error naming it -
// what a photos screen needs for an offline share, where an empty list would
// say "No photos" and hide the failure. A local missing source is still an
// ordinary empty list for list_photos(), which keeps every old caller.
Result<std::vector<PhotoEntry>> list_photos_checked(const PhotoSource& source,
                                                    bool include_splats = false);

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

// One level of `dir`: its sub-folders (names starting with '.' skipped), then
// its photo files (is_photo_file), then its splat files (is_splat_file), each
// group sorted by name ignoring case, at most 500 entries in all. A missing or
// unreadable folder yields an empty list.
std::vector<FolderEntry> list_folder(const Path& dir);

// Every splat file at most `max_depth` levels below `dir`, name order, at most
// `max_files`. The picker's Square on a folder row imports exactly this list.
std::vector<Path> list_splat_files(const Path& dir, int max_depth = 3,
                                   std::size_t max_files = 50);

}  // namespace splat
