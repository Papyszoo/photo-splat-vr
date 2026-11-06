// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/nfs.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#if defined(SPLAT_HAVE_NFS)
#include <nfsc/libnfs.h>
#endif

#include "vfs/net_state.h"
#include "vfs/resolve.h"

namespace splat::vfs {
namespace {

// One NFS READ or WRITE request. libnfs caps the request at the server's
// rtmax/wtmax; 1 MiB is what a NAS answers without renegotiating and matches
// the SMB adapter's chunk.
constexpr size_t kChunk = 1u << 20;

// How long a failed mount is remembered before the location retries on its own
// (console pass 3, L1: a NAS that was still booting must connect without a
// restart). The mount, and the screen's Test, clear it at once.
constexpr std::chrono::seconds kConnectRetry(5);

// libnfs's RPC program versions. The installed header (include/nfsc/libnfs.h)
// documents NFS_V3/NFS_V4 but only defines the NFS_V4_2 dialect selector; the
// two plain numbers are the ones nfs_set_version() switches on (private
// nfs/libnfs-raw-nfs.h, nfs4/libnfs-raw-nfs4.h).
constexpr int kNfsVersion3 = 3;
constexpr int kNfsVersion4 = 4;

std::string trim_slashes(std::string text) {
    while (!text.empty() && text.front() == '/') text.erase(text.begin());
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

// The export the location names, as libnfs's mount argument wants it: a path
// starting at "/" ("/export"), with "/" for a server that exports its root.
std::string export_path(const Location& location) {
    const std::string share = trim_slashes(location.share);
    return share.empty() ? "/" : "/" + share;
}

// The path inside the export: the location's sub-path, then the VFS relative
// path. Always absolute; "" and "/" are the export root.
std::string nfs_path(const Location& location, const std::string& relative) {
    const std::string base = trim_slashes(location.path);
    const std::string tail = trim_slashes(relative);
    if (base.empty() && tail.empty()) return "/";
    if (base.empty()) return "/" + tail;
    if (tail.empty()) return "/" + base;
    return "/" + base + "/" + tail;
}

#if defined(SPLAT_HAVE_NFS)

class NfsFileSystem : public IFileSystem {
public:
    explicit NfsFileSystem(Location location) : location_(std::move(location)) {}
    ~NfsFileSystem() override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    std::string id() const override { return location_.id; }
    std::string describe() const override { return nfs_url(location_); }
    bool writable() const override { return true; }

    void reset_connection() override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        connect_error_.clear();
        set_health_error(std::string());
        error_.clear();
    }

    std::string connection_error() const override {
        // Only the health mutex: mutex_ is held for a whole listing, upload or
        // 5 s mount timeout, and the UI thread calls this every tick (SPEC 3.1:
        // the UI thread must never wait on a network call).
        std::lock_guard<std::mutex> lock(health_mutex_);
        return health_error_;
    }

    Result<std::vector<uint8_t>> read(const std::string& relative, size_t max_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Result<std::vector<uint8_t>>::failure(error_);

        struct nfs_stat_64 info {};
        const int status = nfs_stat64(nfs_, nfs_path(location_, relative).c_str(), &info);
        if (status < 0) return Result<std::vector<uint8_t>>::failure(failure("cannot stat " + relative));
        if (max_bytes != 0 && info.nfs_size > max_bytes) {
            return Result<std::vector<uint8_t>>::failure(
                relative + " is " + std::to_string(info.nfs_size) + " bytes, larger than the " +
                std::to_string(max_bytes) + " bytes limit");
        }
        return read_locked(relative, 0, static_cast<size_t>(info.nfs_size));
    }

    Result<std::vector<uint8_t>> read_range(const std::string& relative, uint64_t offset,
                                            size_t length) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Result<std::vector<uint8_t>>::failure(error_);
        return read_locked(relative, offset, length);
    }

    Status write(const std::string& relative, const uint8_t* data, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        const std::string path = nfs_path(location_, relative);

        struct nfsfh* handle = nullptr;
        // nfs_creat() is the create (O_CREAT|O_TRUNC) call; libnfs 8.0.0 has no
        // nfs_create(). The mode is what a scene/photo file wants.
        if (nfs_creat(nfs_, path.c_str(), 0644, &handle) < 0) {
            return Status::error(failure("cannot create " + relative));
        }

        size_t written_total = 0;
        while (written_total < size) {
            const size_t chunk = std::min(kChunk, size - written_total);
            const int written = nfs_pwrite(nfs_, handle, data + written_total, chunk, written_total);
            if (written <= 0) {
                (void)nfs_close(nfs_, handle);
                return Status::error(failure("write failed for " + relative));
            }
            written_total += static_cast<size_t>(written);
        }
        (void)nfs_fsync(nfs_, handle);
        if (nfs_close(nfs_, handle) < 0) {
            return Status::error(failure("close failed for " + relative));
        }
        return Status::success();
    }

    Status list(const std::string& relative, std::vector<DirEntry>* entries) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        return list_locked(relative, entries);
    }

    Status stat(const std::string& relative, FileStat* out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        *out = FileStat{};
        if (!connect_locked()) return Status::error(error_);

        struct nfs_stat_64 info {};
        const int status = nfs_stat64(nfs_, nfs_path(location_, relative).c_str(), &info);
        if (status < 0) {
            // A missing path is a normal answer; anything else is an error.
            if (status == -ENOENT || status == -ENOTDIR) return Status::success();
            return Status::error(failure("cannot stat " + relative));
        }
        out->exists = true;
        out->directory = S_ISDIR(info.nfs_mode);
        out->regular = S_ISREG(info.nfs_mode);
        out->size = info.nfs_size;
        out->mtime = static_cast<int64_t>(info.nfs_mtime);
        return Status::success();
    }

    Status make_dirs(const std::string& relative) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        const std::string path = nfs_path(location_, relative);
        if (path == "/") return Status::success();

        // One component at a time: NFS has no -p, and an existing component is
        // success because every writer calls this on a folder it may have made.
        std::string built;
        std::size_t start = 1;
        while (start <= path.size()) {
            const std::size_t slash = path.find('/', start);
            const std::string piece =
                path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (!piece.empty()) {
                built += "/" + piece;
                if (nfs_mkdir(nfs_, built.c_str()) < 0) {
                    struct nfs_stat_64 info {};
                    if (nfs_stat64(nfs_, built.c_str(), &info) < 0 || !S_ISDIR(info.nfs_mode)) {
                        return Status::error(failure("cannot create " + relative));
                    }
                }
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return Status::success();
    }

    Status remove_file(const std::string& relative) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        if (nfs_unlink(nfs_, nfs_path(location_, relative).c_str()) < 0) {
            return Status::error(failure("cannot remove " + relative));
        }
        return Status::success();
    }

    Status remove_tree(const std::string& relative) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        return remove_tree_locked(relative);
    }

    Status rename(const std::string& from, const std::string& to) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        if (nfs_rename(nfs_, nfs_path(location_, from).c_str(),
                       nfs_path(location_, to).c_str()) < 0) {
            return Status::error(failure("cannot rename " + from + " to " + to));
        }
        return Status::success();
    }

private:
    // The error string for the last libnfs call, always naming the location.
    std::string failure(const std::string& what) const {
        const char* nfs = nfs_ != nullptr ? nfs_get_error(nfs_) : nullptr;
        std::string message = location_.id + ": " + what;
        if (nfs != nullptr && nfs[0] != '\0') message += ": " + std::string(nfs);
        return message;
    }

    bool connect_locked() {
        if (nfs_ != nullptr) return true;
        if (!connect_error_.empty()) {
            // Retried after a short cooldown, or immediately after the mount
            // reset it (the Locations screen's Test).
            if (std::chrono::steady_clock::now() - connect_error_at_ < kConnectRetry) return false;
            connect_error_.clear();
            set_health_error(std::string());
        }
        // P2: a dead console network stack is named as the stack, not as an
        // unreachable export, and it is asked to come up again on this attempt
        // (L1) instead of being cached for the whole process.
        const std::string stack = net_stack_error();
        if (!stack.empty() && !retry_net_stack()) {
            connect_error_ = location_.id + ": no network stack: " + stack;
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_health_error(connect_error_);
            return false;
        }

        nfs_ = nfs_init_context();
        if (nfs_ == nullptr) {
            connect_error_ = location_.id + ": cannot create an NFS context";
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_health_error(connect_error_);
            return false;
        }
        // AUTH_SYS: the only credentials NFSv3 has. 65534 (nobody/nogroup) is
        // what an unprivileged client uses; the location can raise it.
        nfs_set_uid(nfs_, static_cast<int>(location_.uid));
        nfs_set_gid(nfs_, static_cast<int>(location_.gid));
        if (nfs_set_version(nfs_, location_.nfs_version == 4 ? kNfsVersion4 : kNfsVersion3) < 0) {
            connect_error_ = failure("cannot select NFSv" + std::to_string(location_.nfs_version));
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_health_error(connect_error_);
            disconnect_locked();
            return false;
        }
        // 5 s, so a switched-off NAS reports instead of hanging the worker; two
        // reconnects cover a share that drops while the console sleeps.
        nfs_set_timeout(nfs_, 5000);
        nfs_set_autoreconnect(nfs_, 2);
        nfs_set_tcp_syncnt(nfs_, 3);
        if (location_.port > 0) {
            // A fixed port means no rpcbind: nfsport for the NFS program,
            // mountport for MOUNT (NFSv3 only, there is no MOUNT in v4).
            nfs_set_nfsport(nfs_, location_.port);
            if (location_.nfs_version != 4) nfs_set_mountport(nfs_, location_.port);
        }

        const std::string export_name = export_path(location_);
        if (nfs_mount(nfs_, location_.host.c_str(), export_name.c_str()) < 0) {
            // A name this process cannot resolve means the export was never
            // contacted: say that instead of blaming the export (P1/P2).
            const std::string resolve_error = take_resolve_error();
            connect_error_ = resolve_error.empty()
                                 ? failure("cannot mount " + location_.host + ":" + export_name)
                                 : location_.id + ": " + resolve_error;
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_health_error(connect_error_);
            disconnect_locked();
            return false;
        }
        mounted_ = true;
        connect_error_.clear();
        set_health_error(std::string());
        error_.clear();
        return true;
    }

    // The copy the UI thread reads; every caller holds mutex_.
    void set_health_error(const std::string& text) {
        std::lock_guard<std::mutex> lock(health_mutex_);
        health_error_ = text;
    }

    void disconnect_locked() {
        if (nfs_ == nullptr) return;
        // Only an export that really mounted is unmounted: nfs_umount() on a
        // context that never connected would open another MOUNT connection.
        if (mounted_) (void)nfs_umount(nfs_);
        mounted_ = false;
        nfs_destroy_context(nfs_);
        nfs_ = nullptr;
    }

    Result<std::vector<uint8_t>> read_locked(const std::string& relative, uint64_t offset,
                                             size_t length) {
        const std::string path = nfs_path(location_, relative);
        struct nfsfh* handle = nullptr;
        if (nfs_open(nfs_, path.c_str(), O_RDONLY, &handle) < 0) {
            return Result<std::vector<uint8_t>>::failure(failure("cannot open " + relative));
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(std::min(length, static_cast<size_t>(1) << 26));
        size_t done = 0;
        while (done < length) {
            const size_t chunk = std::min(kChunk, length - done);
            bytes.resize(done + chunk);
            const int got = nfs_pread(nfs_, handle, bytes.data() + done, chunk, offset + done);
            if (got < 0) {
                (void)nfs_close(nfs_, handle);
                return Result<std::vector<uint8_t>>::failure(failure("cannot read " + relative));
            }
            bytes.resize(done + static_cast<size_t>(got));
            done += static_cast<size_t>(got);
            if (static_cast<size_t>(got) < chunk) break;  // end of file
        }
        (void)nfs_close(nfs_, handle);
        return Result<std::vector<uint8_t>>::success(std::move(bytes));
    }

    Status list_locked(const std::string& relative, std::vector<DirEntry>* entries) {
        const std::string path = nfs_path(location_, relative);
        struct nfsdir* dir = nullptr;
        if (nfs_opendir(nfs_, path.c_str(), &dir) < 0) {
            return Status::error(failure("cannot list " + relative));
        }

        for (;;) {
            struct nfsdirent* item = nfs_readdir(nfs_, dir);
            if (item == nullptr) break;
            const std::string name = item->name != nullptr ? item->name : "";
            // Some servers return "." and "..", some do not: skip them when
            // they are there and never pre-read a first entry.
            if (name.empty() || name == "." || name == "..") continue;
            DirEntry entry;
            entry.name = name;
            // libnfs ORs the S_IF* bit into mode for both v3 (READDIRPLUS) and
            // v4, so the entry type is exact without a stat per entry.
            entry.directory = S_ISDIR(item->mode);
            entry.symlink = S_ISLNK(item->mode);
            entry.size = item->size;
            entry.mtime = static_cast<int64_t>(item->mtime.tv_sec);
            entries->push_back(std::move(entry));
        }
        nfs_closedir(nfs_, dir);
        return Status::success();
    }

    Status remove_tree_locked(const std::string& relative) {
        std::vector<DirEntry> entries;
        const Status listed = list_locked(relative, &entries);
        if (!listed.ok()) return listed;
        for (const DirEntry& entry : entries) {
            const std::string child = relative.empty() ? entry.name : relative + "/" + entry.name;
            if (entry.directory) {
                const Status removed = remove_tree_locked(child);
                if (!removed.ok()) return removed;
                continue;
            }
            if (nfs_unlink(nfs_, nfs_path(location_, child).c_str()) < 0) {
                return Status::error(failure("cannot remove " + child));
            }
        }
        if (!relative.empty() && nfs_rmdir(nfs_, nfs_path(location_, relative).c_str()) < 0) {
            return Status::error(failure("cannot remove " + relative));
        }
        return Status::success();
    }

    Location location_;
    mutable std::mutex mutex_;
    struct nfs_context* nfs_ = nullptr;
    bool mounted_ = false;       // the export answered nfs_mount()
    std::string error_;          // the last failure, returned by the free functions
    // A failed mount, held for kConnectRetry. connection_error() reads the
    // mirror under health_mutex_ only, so the UI thread never waits for
    // mutex_ (held through a listing or a 5 s mount); every update of the
    // mirror happens while mutex_ is held.
    mutable std::mutex health_mutex_;
    std::string health_error_;
    std::string connect_error_;
    std::chrono::steady_clock::time_point connect_error_at_{};
};

#endif  // SPLAT_HAVE_NFS

}  // namespace

std::string nfs_url(const Location& location) {
    const std::string version = location.nfs_version == 4 ? "4" : "3";
    std::string url = "nfs://" + location.host;
    const std::string share = trim_slashes(location.share);
    if (!share.empty()) url += "/" + share;
    const std::string path = trim_slashes(location.path);
    if (!path.empty()) url += "/" + path;
    url += "?version=" + version;
    if (location.port > 0) {
        url += "&nfsport=" + std::to_string(location.port);
        // NFSv4 has no MOUNT program, so a mount port would be a lie.
        if (location.nfs_version != 4) url += "&mountport=" + std::to_string(location.port);
    }
    url += "&uid=" + std::to_string(location.uid);
    url += "&gid=" + std::to_string(location.gid);
    return url;
}

std::shared_ptr<IFileSystem> make_nfs_file_system(const Location& location, std::string* error) {
    if (location.host.empty() || location.share.empty()) {
        if (error != nullptr) {
            *error = location.id + ": an NFS location needs a host and an export";
        }
        return nullptr;
    }
#if defined(SPLAT_HAVE_NFS)
    return std::make_shared<NfsFileSystem>(location);
#else
    if (error != nullptr) {
        *error = location.id + ": NFS support is not built into this version";
    }
    return nullptr;
#endif
}

}  // namespace splat::vfs
