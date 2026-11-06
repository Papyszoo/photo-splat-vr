// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/io.h"

#include <algorithm>
#include <utility>

#include "vfs/local_fs.h"

namespace splat::vfs {
namespace {

LocalFileSystem& local() {
    static LocalFileSystem instance;
    return instance;
}

// The sidecar a replace through an adapter without rename-over leaves when it
// is interrupted between the two renames: the old file, whole, under this name.
// A read of the missing name puts it back before reading, so an interrupted
// turn still opens.
constexpr const char* kReplaceBackupSuffix = ".replace-old";

bool has_suffix(const std::string& text, const char* suffix) {
    const std::string tail(suffix);
    if (text.size() < tail.size()) return false;
    return text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// If `path` is missing but its replace backup is there, rename the backup back.
// Nothing happens for an adapter whose rename already replaces (it never makes
// a backup), so a local/NFS read pays no extra call.
void recover_replace_backup(IFileSystem& file_system, const std::string& path) {
    if (file_system.rename_replaces()) return;
    FileStat target;
    if (!file_system.stat(path, &target).ok() || target.exists) return;
    const std::string backup = path + kReplaceBackupSuffix;
    FileStat saved;
    if (!file_system.stat(backup, &saved).ok() || !saved.exists) return;
    (void)file_system.rename(backup, path);
}

// Replaces `to` with `from` on an adapter that cannot rename over an existing
// name (SMB): the old target is moved to a sidecar first, the new file takes its
// place, and the sidecar is removed. On a failed second rename the sidecar goes
// back, so the target is the old file again. An interruption between the two
// renames leaves the old bytes in the sidecar; recover_replace_backup() puts
// them back on the next read. Returns the adapter error when nothing changed.
Status exchange_replace(IFileSystem& file_system, const std::string& from,
                        const std::string& to) {
    FileStat target;
    const Status looked = file_system.stat(to, &target);
    if (!looked.ok()) return looked;
    if (!target.exists) return file_system.rename(from, to);

    const std::string backup = to + kReplaceBackupSuffix;
    FileStat stale;
    const Status stale_looked = file_system.stat(backup, &stale);
    if (!stale_looked.ok()) return stale_looked;
    if (stale.exists) {
        const Status removed = file_system.remove_file(backup);
        if (!removed.ok()) return removed;
    }
    const Status moved_old = file_system.rename(to, backup);
    if (!moved_old.ok()) return moved_old;
    const Status moved_new = file_system.rename(from, to);
    if (!moved_new.ok()) {
        const Status restored = file_system.rename(backup, to);
        return Status::error(moved_new.message() +
                             (restored.ok() ? std::string()
                                            : "; the old file is at " + backup + " (" +
                                                  restored.message() + ")"));
    }
    (void)file_system.remove_file(backup);  // a leftover copy is not a failure
    return Status::success();
}

// The file system a path names. `relative` is what the adapter receives. An
// unknown location id fails here, with a message that names it, so a dropped
// mount never turns into a silent local read.
Status route(const Path& path, std::shared_ptr<IFileSystem>* file_system, std::string* relative) {
    std::string id;
    if (!split(path, &id, relative)) {
        file_system->reset();
        return Status::success();
    }
    std::shared_ptr<IFileSystem> found = file_systems().find(id);
    if (!found) {
        return Status::error("unknown location \"" + id + "\" for " + path.string() +
                             " (add it in Settings > Network locations)");
    }
    *file_system = std::move(found);
    return Status::success();
}

Status copy_into(const Path& from, const Path& to) {
    // The destination directory exists before any file in it is written, so a
    // copy that fails halfway leaves a folder the caller can inspect.
    const Status made = make_dirs(to);
    if (!made.ok()) return made;

    std::vector<DirEntry> entries;
    const Status listed = list(from, &entries);
    if (!listed.ok()) return listed;

    for (const DirEntry& entry : entries) {
        if (entry.symlink) continue;  // never follow a link out of the tree
        const Path source = from / entry.name;
        const Path target = to / entry.name;
        if (entry.directory) {
            const Status copied = copy_into(source, target);
            if (!copied.ok()) return copied;
            continue;
        }
        const Result<std::vector<uint8_t>> bytes = read_file(source);
        if (!bytes.ok()) return Status::error(bytes.message());
        const Status written = write_file(target, bytes.value());
        if (!written.ok()) return written;
    }
    return Status::success();
}

}  // namespace

bool valid_location_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    if (id.front() < 'a' || id.front() > 'z') return false;
    for (const char c : id) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!allowed) return false;
    }
    return true;
}

bool split(const Path& path, std::string* id, std::string* relative) {
    const std::string text = path.string();
    const std::size_t colon = text.find(":/");
    if (colon == std::string::npos) return false;
    const std::string candidate = text.substr(0, colon);
    if (!valid_location_id(candidate)) return false;
    if (id != nullptr) *id = candidate;
    if (relative != nullptr) *relative = text.substr(colon + 2);
    return true;
}

Path join(const std::string& id, const std::string& relative) {
    std::size_t start = 0;
    while (start < relative.size() && relative[start] == '/') ++start;
    return Path(id + ":/" + relative.substr(start));
}

bool is_remote(const Path& path) { return split(path, nullptr, nullptr); }

std::string location_id(const Path& path) {
    std::string id;
    return split(path, &id, nullptr) ? id : std::string();
}

Path path_key(const Path& path) {
    if (is_remote(path)) {
        std::string id;
        std::string relative;
        if (split(path, &id, &relative)) {
            // A trailing separator names the same folder: the key is the
            // location and the path inside it, not the caller's spelling.
            while (!relative.empty() && relative.back() == '/') relative.pop_back();
            // Deliberately not join(): that spells a *location path*
            // ("smb0:/library/…"); the key joins with a bare colon
            // ("smb0:library/…"), so a stored key can never be mistaken for a
            // path that routes to an adapter.
            return Path(id + ":" + relative);
        }
    }
    Path absolute;
    if (normal_absolute(path, absolute)) return absolute;
    return path.lexically_normal();
}

Result<std::vector<uint8_t>> IFileSystem::read_range(const std::string& relative, uint64_t offset,
                                                     size_t length) {
    if (length == 0) return Result<std::vector<uint8_t>>::success({});
    // No limit: read() refuses a file larger than max_bytes, and a range read
    // of a big file is exactly the case this default exists for. The short
    // window is sliced out below.
    const Result<std::vector<uint8_t>> bytes = read(relative, 0);
    if (!bytes.ok()) return bytes;
    const std::vector<uint8_t>& all = bytes.value();
    if (offset >= all.size()) return Result<std::vector<uint8_t>>::success({});
    const std::size_t from = static_cast<std::size_t>(offset);
    const std::size_t count = std::min(length, all.size() - from);
    return Result<std::vector<uint8_t>>::success(
        std::vector<uint8_t>(all.begin() + static_cast<std::ptrdiff_t>(from),
                             all.begin() + static_cast<std::ptrdiff_t>(from + count)));
}

void FileSystems::add(std::shared_ptr<IFileSystem> file_system) {
    if (!file_system) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = file_system->id();
    for (std::shared_ptr<IFileSystem>& mounted : mounted_) {
        if (mounted->id() == id) {
            mounted = std::move(file_system);
            return;
        }
    }
    mounted_.push_back(std::move(file_system));
}

void FileSystems::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    mounted_.erase(std::remove_if(mounted_.begin(), mounted_.end(),
                                  [&id](const std::shared_ptr<IFileSystem>& mounted) {
                                      return mounted->id() == id;
                                  }),
                   mounted_.end());
}

std::shared_ptr<IFileSystem> FileSystems::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<IFileSystem>& mounted : mounted_) {
        if (mounted->id() == id) return mounted;
    }
    return nullptr;
}

std::vector<std::shared_ptr<IFileSystem>> FileSystems::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mounted_;
}

void FileSystems::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    mounted_.clear();
}

FileSystems& file_systems() {
    static FileSystems instance;
    return instance;
}

IFileSystem& local_file_system() { return local(); }

Result<std::vector<uint8_t>> read_file(const Path& path, size_t max_bytes) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return Result<std::vector<uint8_t>>::failure(routed.message());
    if (!mounted) return local().read(path.string(), max_bytes);
    Result<std::vector<uint8_t>> bytes = mounted->read(relative, max_bytes);
    if (!bytes.ok() && !mounted->rename_replaces()) {
        // An interrupted replace leaves the target missing and the old bytes in
        // its sidecar; put them back and read the file that was never lost.
        recover_replace_backup(*mounted, relative);
        bytes = mounted->read(relative, max_bytes);
    }
    return bytes;
}

Status read_text_file(const Path& path, std::string* out, size_t max_bytes) {
    if (out == nullptr) return Status::error("read_text_file needs an output string");
    const Result<std::vector<uint8_t>> bytes = read_file(path, max_bytes);
    if (!bytes.ok()) return Status::error(bytes.message());
    out->assign(bytes.value().begin(), bytes.value().end());
    return Status::success();
}

Result<std::vector<uint8_t>> read_range(const Path& path, uint64_t offset, size_t length) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return Result<std::vector<uint8_t>>::failure(routed.message());
    if (!mounted) return local().read_range(path.string(), offset, length);
    Result<std::vector<uint8_t>> bytes = mounted->read_range(relative, offset, length);
    if (!bytes.ok() && !mounted->rename_replaces()) {
        recover_replace_backup(*mounted, relative);
        bytes = mounted->read_range(relative, offset, length);
    }
    return bytes;
}

Status write_file(const Path& path, const uint8_t* data, size_t size) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return routed;
    if (!mounted) {
        // A local writer creates the parent: a scene folder's files are written
        // after make_dirs() anyway, but a state file in a fresh directory is not.
        const std::string parent = path.parent_path().string();
        if (!parent.empty()) {
            const Status made = local().make_dirs(parent);
            if (!made.ok()) return made;
        }
        return local().write(path.string(), data, size);
    }
    return mounted->write(relative, data, size);
}

Status list(const Path& directory, std::vector<DirEntry>* entries) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(directory, &mounted, &relative);
    if (!routed.ok()) return routed;
    const Status listed =
        mounted ? mounted->list(relative, entries) : local().list(directory.string(), entries);
    if (!listed.ok() || entries == nullptr) return listed;
    // A replace sidecar is internal, never a photo or a scene the user may
    // pick; the target it belongs to is restored on its next read.
    entries->erase(std::remove_if(entries->begin(), entries->end(),
                                  [](const DirEntry& entry) {
                                      return has_suffix(entry.name, kReplaceBackupSuffix);
                                  }),
                   entries->end());
    return Status::success();
}

Status stat(const Path& path, FileStat* out) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return routed;
    if (!mounted) return local().stat(path.string(), out);
    const Status looked = mounted->stat(relative, out);
    if (!looked.ok() || out->exists || mounted->rename_replaces()) return looked;
    recover_replace_backup(*mounted, relative);
    return mounted->stat(relative, out);
}

Status make_dirs(const Path& path) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return routed;
    if (!mounted) return local().make_dirs(path.string());
    return mounted->make_dirs(relative);
}

Status remove_file(const Path& path) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return routed;
    if (!mounted) return local().remove_file(path.string());
    return mounted->remove_file(relative);
}

Status remove_tree(const Path& path) {
    std::shared_ptr<IFileSystem> mounted;
    std::string relative;
    const Status routed = route(path, &mounted, &relative);
    if (!routed.ok()) return routed;
    if (!mounted) return local().remove_tree(path.string());
    return mounted->remove_tree(relative);
}

Status rename_file(const Path& from, const Path& to) {
    if (from.empty() || to.empty()) return Status::error("rename needs two paths");
    std::shared_ptr<IFileSystem> from_mounted;
    std::shared_ptr<IFileSystem> to_mounted;
    std::string from_relative;
    std::string to_relative;
    const Status from_routed = route(from, &from_mounted, &from_relative);
    if (!from_routed.ok()) return from_routed;
    const Status to_routed = route(to, &to_mounted, &to_relative);
    if (!to_routed.ok()) return to_routed;
    // One adapter does both sides: a local file cannot be renamed onto a share
    // and two shares cannot rename into each other (that would have to copy).
    if (from_mounted != to_mounted) {
        return Status::error("cannot rename " + from.string() + " to " + to.string() +
                             ": the two paths are on different file systems");
    }
    if (!from_mounted) return local().rename(from.string(), to.string());
    // A crash mid-publish may have left the old target in its sidecar; restore
    // it before replacing it again.
    recover_replace_backup(*from_mounted, to_relative);
    if (!from_mounted->rename_replaces()) {
        return exchange_replace(*from_mounted, from_relative, to_relative);
    }
    return from_mounted->rename(from_relative, to_relative);
}

Path temporary_beside(const Path& path, const char* suffix) {
    Path temporary = path;
    temporary += suffix;
    return temporary;
}

Status replace_file(const Path& path, const uint8_t* data, size_t size) {
    const Path temporary = temporary_beside(path);
    (void)remove_file(temporary);  // a stale temp from an interrupted run
    const Status written = write_file(temporary, data, size);
    if (!written.ok()) {
        (void)remove_file(temporary);
        return written;
    }
    const Status renamed = rename_file(temporary, path);
    if (!renamed.ok()) {
        (void)remove_file(temporary);
        return renamed;
    }
    return Status::success();
}

Status copy_tree(const Path& from, const Path& to) {
    if (from.empty() || to.empty()) return Status::error("copy_tree needs two paths");
    return copy_into(from, to);
}

}  // namespace splat::vfs
