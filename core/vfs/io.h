// The file-system interface the gallery, the scene readers and the job runner
// use, plus the path grammar that names a network location.
//
// A path is either a plain OS path (`/data/ps5-splat-vr/scenes`) or a location
// path `<id>:/<relative>` (`smb0:/photos/2026-10`). `id` is the stable id a
// Location got from its store (core/vfs/location.h): a lowercase letter
// followed by lowercase letters and digits. Anything else - including a Windows
// drive letter, which is uppercase - is a local OS path, so the grammar cannot
// shadow a real file name.
//
// The free functions below dispatch on that split: a local path goes to
// LocalFileSystem (the only place in core/gallery, core/scene and core/jobs
// that still calls std::filesystem), a location path to the IFileSystem
// registered under its id (core/vfs/smb.cpp, and later nfs.cpp). An unknown id
// is an error that names it, never a silent local read.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat::vfs {

// One entry of a directory listing. `symlink` is the type the directory read
// reported for the entry (readdir's d_type); it is what scene_location() uses to
// refuse a symlinked scene, because a PS5 title is refused lstat.
struct DirEntry {
    std::string name;
    bool directory = false;
    bool symlink = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix seconds; 0 when it cannot be read
};

// The type and size of one path. `exists == false` is a normal answer (a
// missing file), not an error.
struct FileStat {
    bool exists = false;
    bool directory = false;
    bool regular = false;
    uint64_t size = 0;
    int64_t mtime = 0;
};

// True for [a-z][a-z0-9]*, at most 32 characters. The id is the first path
// component of a location path ("smb0" in "smb0:/a/b").
bool valid_location_id(const std::string& id);

// Splits `<id>:/<relative>`. False - and the path is local - when it does not
// match the grammar. `relative` has no leading slash ("" is the location root).
bool split(const Path& path, std::string* id, std::string* relative);

// `join("smb0", "a/b")` is `smb0:/a/b`; a leading slash on `relative` is
// dropped, and an empty one gives the location root.
Path join(const std::string& id, const std::string& relative);

bool is_remote(const Path& path);

// The location id of a location path, "" for a local one.
std::string location_id(const Path& path);

// A stable, canonical key for a file or folder: a network path keys by its
// location id plus the path inside it ("smb0:photos/a.jpg"), a local one by its
// lexical absolute path. The same scene or photo spelled with a trailing
// separator keys the same; nothing here touches the filesystem (no realpath on
// a PS5 title). Settings that belong to a file on this console (a saved photo
// turn, a fallback scene scale) use this one rule.
Path path_key(const Path& path);

// One mounted file system. `relative` is the path after `<id>:/`: "" is its
// root, "a/b" a child. A local file system receives the whole OS path instead,
// because that is what it was handed by its callers.
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual std::string id() const = 0;
    // One line for the locations screen ("smb://nas/Photos").
    virtual std::string describe() const = 0;
    // False for a read-only mount; mutations then return an error instead of
    // being attempted.
    virtual bool writable() const = 0;

    virtual Result<std::vector<uint8_t>> read(const std::string& relative,
                                              size_t max_bytes) = 0;
    // The default reads the whole file and slices it; an adapter with a real
    // ranged read (smb2_pread, nfs_pread) overrides it.
    virtual Result<std::vector<uint8_t>> read_range(const std::string& relative, uint64_t offset,
                                                    size_t length);
    virtual Status write(const std::string& relative, const uint8_t* data, size_t size) = 0;
    virtual Status list(const std::string& relative, std::vector<DirEntry>* entries) = 0;
    virtual Status stat(const std::string& relative, FileStat* out) = 0;
    virtual Status make_dirs(const std::string& relative) = 0;
    virtual Status remove_file(const std::string& relative) = 0;
    virtual Status remove_tree(const std::string& relative) = 0;
    // Replaces `to` with `from` in one step. The default refuses, so an adapter
    // that does not have one (and a test fake that does not need it) is not
    // forced to grow a method; the local, SMB and NFS adapters implement it.
    virtual Status rename(const std::string& /*from*/, const std::string& /*to*/) {
        return Status::error("this location cannot rename files");
    }
    // False when rename() cannot replace an existing target. libsmb2 6.3's
    // smb2_rename() sends ReplaceIfExists=0, so every SMB server refuses a
    // target that is already there; vfs::rename_file() then publishes through a
    // sidecar backup so a photo or scene turn still overrides the old file.
    virtual bool rename_replaces() const { return true; }

    // Drops any cached connection and any latched connect error, so the next
    // call connects again. The location mount calls it before its probe: a NAS
    // that was still booting when the app started must not stay offline for the
    // life of the process, and the Locations screen's Test is the same mount
    // (console pass 3, L1). A local file system has nothing to reset.
    virtual void reset_connection() {}

    // The last failure that made this adapter drop its connection, or "" when
    // its connection is up (or it has none). The Locations screen shows it
    // instead of a stale "ready": one failed SMB reply used to leave a location
    // unusable while the screen still said ready (smb-e2e, BUG 1).
    virtual std::string connection_error() const { return std::string(); }
};

// The mounted file systems, keyed by id. A location adapter registers itself
// after a successful mount (LocationStore::mount_all); the free functions find
// it here. Thread-safe: a listing worker and the UI read it at the same time.
class FileSystems {
public:
    void add(std::shared_ptr<IFileSystem> file_system);
    void remove(const std::string& id);
    std::shared_ptr<IFileSystem> find(const std::string& id) const;
    std::vector<std::shared_ptr<IFileSystem>> all() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IFileSystem>> mounted_;
};

// The one registry the app uses (the tests register their fakes in it).
FileSystems& file_systems();

// The local adapter, registered under no id: it is what a plain path reaches.
IFileSystem& local_file_system();

// --- the free functions the rest of core calls -----------------------------

// The whole file, at most `max_bytes` bytes (0 = no limit). The error names the
// path and, for a location path it cannot reach, the location id.
Result<std::vector<uint8_t>> read_file(const Path& path, size_t max_bytes = 0);
// The whole file as text, through the same routing. The one reader the state
// files and meta.json use, so a network path never reaches an OS call.
Status read_text_file(const Path& path, std::string* out, size_t max_bytes = 0);
// A byte range: the format sniff of a multi-GB splat file reads its first
// kilobytes, not the file. A short file yields what it has.
Result<std::vector<uint8_t>> read_range(const Path& path, uint64_t offset, size_t length);
Status write_file(const Path& path, const uint8_t* data, size_t size);
inline Status write_file(const Path& path, const std::vector<uint8_t>& bytes) {
    return write_file(path, bytes.data(), bytes.size());
}

// One directory level. A missing directory is an error naming it; a listing of
// "." on a location is its root.
Status list(const Path& directory, std::vector<DirEntry>* entries);
Status stat(const Path& path, FileStat* out);
Status make_dirs(const Path& path);
Status remove_file(const Path& path);
Status remove_tree(const Path& path);
// The name a replace writes beside `path` before renaming over it. The suffix
// is internal; every caller that writes-and-renames shares this rule.
Path temporary_beside(const Path& path, const char* suffix = ".rotate-tmp");
// Replaces one file through the VFS: the bytes go to temporary_beside(path),
// which is then renamed over it; on any failure the temporary is removed and
// the old file is left whole. For a single file; a folder whose files must be
// published in a fixed order writes its temporaries first and renames them
// itself (core/scene/scene_rotate.cpp).
Status replace_file(const Path& path, const uint8_t* data, size_t size);
inline Status replace_file(const Path& path, const std::vector<uint8_t>& bytes) {
    return replace_file(path, bytes.data(), bytes.size());
}
// Replaces `to` with `from`, through the same adapter the two paths route to.
// Both must name the same location: a rename across a location boundary is
// refused instead of silently copying. An adapter whose rename cannot replace an
// existing name (SMB: libsmb2 sends ReplaceIfExists=0) publishes through a
// sidecar backup (see IFileSystem::rename_replaces); a read of a name left
// missing by an interrupted swap puts the old file back.
Status rename_file(const Path& from, const Path& to);

// Copies a directory tree: every file and sub-directory below `from` is created
// below `to`. The destination may be a location path; the target directory is
// created before its files are written, so a fake can see make_dirs() first.
// A file that exists at the destination is overwritten.
Status copy_tree(const Path& from, const Path& to);

}  // namespace splat::vfs
