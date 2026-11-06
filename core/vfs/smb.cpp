// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/smb.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>

#include "vfs/net_state.h"
#include "vfs/resolve.h"

#if defined(SPLAT_HAVE_SMB2)
// smb2.h carries the protocol constants and the error codes the client header
// uses; libsmb2's own examples include the pair in this order.
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#endif

namespace splat::vfs {
namespace {

// One ranged read request or write chunk. SMB2 lets the server cap this, and
// 1 MiB is what every NAS accepts without renegotiating for a scene folder's
// 56 MB scene.psplat.
constexpr size_t kChunk = 1u << 20;

// How long a failed connect is remembered before the location retries on its
// own. Long enough that a gallery of tiles does not each pay the 5 s timeout,
// short enough that a NAS which was still booting connects without a restart.
constexpr std::chrono::seconds kConnectRetry(5);

// The path inside the share: the location's sub-path, then the VFS relative
// path. Relative to the share root, with "" for the root itself.
//
// The leading "/" the adapter used to carry is not a path separator to
// libsmb2: it is part of the name, and Samba refuses it (STATUS_INVALID_
// PARAMETER for every list and stat). Apple's smbd and impacket's smbserver
// tolerate it, which is why the absolute form went unnoticed until a Samba
// server was tested (stage N, item 10). The canonical SMB2 form is the one
// every server accepts, so this is it.
std::string share_path(const Location& location, const std::string& relative) {
    std::string base = location.path;
    while (!base.empty() && base.front() == '/') base.erase(base.begin());
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string tail = relative;
    while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
    while (!tail.empty() && tail.back() == '/') tail.pop_back();

    if (base.empty() && tail.empty()) return std::string();
    if (base.empty()) return tail;
    if (tail.empty()) return base;
    return base + "/" + tail;
}

std::string host_literal(const std::string& host) {
    // libsmb2 splits the server string at the first ':' after an optional
    // "[...]" prefix (lib/socket.c, smb2_connect_async), so an IPv6 literal
    // must be bracketed or "::1" is read as host "" and port ":1". A location
    // written by hand may already carry the brackets; keep them as they are.
    if (host.find(':') == std::string::npos) return host;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') return host;
    return "[" + host + "]";
}

std::string with_port(const Location& location) {
    const std::string host = host_literal(location.host);
    if (location.port <= 0 || location.port == 445) return host;
    return host + ":" + std::to_string(location.port);
}

#if defined(SPLAT_HAVE_SMB2)

// smb_missing_name() lives in the public part of this file (see smb.h): the
// class below and tests/vfs_smb_test.cpp both read it.

class SmbFileSystem : public IFileSystem {
public:
    explicit SmbFileSystem(Location location) : location_(std::move(location)) {}
    ~SmbFileSystem() override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
    }

    std::string id() const override { return location_.id; }
    std::string describe() const override { return smb_url(location_); }
    bool writable() const override { return true; }

    void reset_connection() override {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        reset_locked();
    }

    std::string connection_error() const override {
        // Only the health mutex, never mutex_: the UI thread calls this every
        // tick, and mutex_ is held for a whole listing, upload or 5 s connect
        // timeout. Blocking there would freeze the panel (SPEC 3.1).
        std::lock_guard<std::mutex> lock(health_mutex_);
        return last_failure_;
    }

    Result<std::vector<uint8_t>> read(const std::string& relative, size_t max_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Result<std::vector<uint8_t>>::failure(error_);

        struct smb2_stat_64 info {};
        const int status =
            smb2_stat(ctx_, share_path(location_, relative).c_str(), &info);
        if (status < 0) {
            if (smb_missing_name(status)) {
                return Result<std::vector<uint8_t>>::failure(location_.id + ": no such file: " +
                                                             relative);
            }
            return Result<std::vector<uint8_t>>::failure(failed("cannot stat " + relative));
        }
        if (max_bytes != 0 && info.smb2_size > max_bytes) {
            return Result<std::vector<uint8_t>>::failure(
                relative + " is " + std::to_string(info.smb2_size) + " bytes, larger than the " +
                std::to_string(max_bytes) + " bytes limit");
        }
        return read_locked(relative, 0, static_cast<size_t>(info.smb2_size));
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
        const std::string path = share_path(location_, relative);
        struct smb2fh* handle = smb2_open(ctx_, path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
        if (handle == nullptr) return Status::error(failed("cannot create " + relative));

        size_t written_total = 0;
        while (written_total < size) {
            const size_t chunk = std::min(kChunk, size - written_total);
            const int written = smb2_pwrite(
                ctx_, handle, data + written_total, static_cast<uint32_t>(chunk), written_total);
            if (written <= 0) {
                (void)smb2_close(ctx_, handle);
                return Status::error(failed("write failed for " + relative));
            }
            written_total += static_cast<size_t>(written);
        }
        (void)smb2_fsync(ctx_, handle);
        if (smb2_close(ctx_, handle) < 0) return Status::error(failed("close failed for " + relative));
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

        struct smb2_stat_64 info {};
        const std::string path = share_path(location_, relative);
        const int status = smb2_stat(ctx_, path.c_str(), &info);
        if (status < 0) {
            // A missing path is a normal answer; anything else is an error.
            // Both signals are read: smb2_stat() reports a missing name only
            // through its return value on libsmb2 6.3, and a server whose error
            // reply libsmb2 rejects (impacket) lands in the error branch, where
            // the caller treats it as "does not exist" and the connection is
            // rebuilt.
            if (smb_missing_name(status)) return Status::success();
            return Status::error(probe_failed("cannot stat " + relative));
        }
        out->exists = true;
        out->directory = info.smb2_type == SMB2_TYPE_DIRECTORY;
        out->regular = info.smb2_type == SMB2_TYPE_FILE;
        out->size = info.smb2_size;
        out->mtime = static_cast<int64_t>(info.smb2_mtime);
        return Status::success();
    }

    Status make_dirs(const std::string& relative) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connect_locked()) return Status::error(error_);
        const std::string path = share_path(location_, relative);
        if (path.empty()) return Status::success();

        // One component at a time: SMB has no -p. Each component is *asked*
        // about first, so a folder that already exists costs no failed request
        // at all - the old code mkdir'ed every component and learned "it is
        // there" from the collision error, which left the connection broken on
        // libsmb2 6.3 (smb-e2e, BUG 2). A stat of a name that is not there yet
        // still answers with an error, and that is unavoidable in SMB; the
        // adapter rebuilds its connection after it, so the mkdir that follows
        // runs on a healthy one.
        std::string built;
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t slash = path.find('/', start);
            const std::string piece = path.substr(start, slash == std::string::npos
                                                             ? std::string::npos
                                                             : slash - start);
            if (!piece.empty()) {
                if (!built.empty()) built += "/";
                built += piece;
                if (!directory_exists_locked(built)) {
                    // The stat above may have cost the connection (a rejected
                    // "no such file" reply); reconnect before the create.
                    if (!connect_locked()) return Status::error(error_);
                    if (smb2_mkdir(ctx_, built.c_str()) < 0) {
                        // Not necessarily a failure: the folder may have
                        // appeared between the stat and the create.
                        if (!directory_exists_locked(built)) {
                            return Status::error(failed("cannot create " + relative));
                        }
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
        if (smb2_unlink(ctx_, share_path(location_, relative).c_str()) < 0) {
            return Status::error(failed("cannot remove " + relative));
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
        if (smb2_rename(ctx_, share_path(location_, from).c_str(),
                        share_path(location_, to).c_str()) < 0) {
            return Status::error(failed("cannot rename " + from + " to " + to));
        }
        return Status::success();
    }
    // libsmb2 6.3 hardcodes ReplaceIfExists=0 in every rename request, so the
    // protocol refuses a target that already exists; vfs::rename_file() swaps
    // through a sidecar backup instead (core/vfs/io.cpp).
    bool rename_replaces() const override { return false; }

private:
    // The error string for the last libsmb2 call, always naming the location.
    std::string failure(const std::string& what) const {
        const char* smb = ctx_ != nullptr ? smb2_get_error(ctx_) : nullptr;
        std::string message = location_.id + ": " + what;
        if (smb != nullptr && smb[0] != '\0') message += ": " + std::string(smb);
        return message;
    }

    // One failed libsmb2 call: build the message, then drop the context so the
    // next call reconnects. libsmb2 6.3 rejects the first server error reply it
    // cannot parse and leaves the reader with its pending iovecs, so every
    // later call on that context answers "No io vectors left to read into";
    // keeping it made one missing file or one existing folder kill the location
    // until the app restarted (smb-e2e, BUG 1).
    std::string failed(const std::string& what) {
        const std::string message = failure(what);
        set_last_failure(message);
        error_ = message;
        drop_locked();
        return message;
    }

    // A stat that could not be answered: the connection is rebuilt, but the
    // location does not count as broken. Absence of a name is a normal answer,
    // and unique_scene_folder() probes one per publish - a screen that flipped
    // to offline for every probe would be worse than useless (smb-e2e, BUG 1).
    std::string probe_failed(const std::string& what) {
        const std::string message = failure(what);
        drop_locked();
        return message;
    }

    // The one writer of last_failure_; every caller already holds mutex_.
    void set_last_failure(const std::string& message) {
        std::lock_guard<std::mutex> lock(health_mutex_);
        last_failure_ = message;
    }

    // Tears the context down without a TREE_DISCONNECT: the reply the caller
    // just got was unreadable, and a disconnect would wait for the same reader.
    // smb2_destroy_context() closes the socket and frees every queue.
    void drop_locked() {
        if (ctx_ == nullptr) return;
        smb2_destroy_context(ctx_);
        ctx_ = nullptr;
    }

    // Everything a fresh connection must not inherit.
    void reset_locked() {
        connect_error_.clear();
        set_last_failure(std::string());
        error_.clear();
    }

    // True when `path` (absolute, inside the share) is a directory. A path that
    // is not there answers false. Asking may cost the connection when the reply
    // cannot be parsed, so it is rebuilt before returning: the caller's mkdir
    // must not run on a dropped context.
    bool directory_exists_locked(const std::string& path) {
        struct smb2_stat_64 info {};
        const int status = smb2_stat(ctx_, path.c_str(), &info);
        if (status == 0) return info.smb2_type == SMB2_TYPE_DIRECTORY;
        // A "no such name" answer is normal here and leaves the connection
        // usable; anything else may have cost it, so the next call reconnects.
        if (smb_missing_name(status)) return false;
        drop_locked();
        return false;
    }

    bool connect_locked() {
        if (ctx_ != nullptr) return true;
        if (!connect_error_.empty()) {
            // A failed connect is held for a few seconds so a gallery of tiles
            // does not each pay the 5 s timeout; after that the location is
            // retried on its own, and reset_connection() (the mount, the
            // screen's Test) clears it at once (console pass 3, L1).
            if (std::chrono::steady_clock::now() - connect_error_at_ < kConnectRetry) return false;
            connect_error_.clear();
        }
        // P2: a console whose network stack never came up cannot reach any
        // share, and the offline line must say so instead of blaming the host.
        // The stack is asked to come up again on this attempt (L1): the init
        // that failed at boot is not final.
        const std::string stack = net_stack_error();
        if (!stack.empty() && !retry_net_stack()) {
            connect_error_ = location_.id + ": no network stack: " + stack;
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_last_failure(connect_error_);
            return false;
        }

        ctx_ = smb2_init_context();
        if (ctx_ == nullptr) {
            connect_error_ = location_.id + ": cannot create an SMB context";
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_last_failure(connect_error_);
            return false;
        }
        // 5 s, so a switched-off NAS reports instead of hanging the worker.
        smb2_set_timeout(ctx_, 5);
        if (location_.encrypt) {
            // Sealing is SMB3-only (AES-128-CCM).
            smb2_set_version(ctx_, SMB2_VERSION_ANY3);
            smb2_set_seal(ctx_, 1);
        }
        if (location_.user.empty()) {
            smb2_set_user(ctx_, "Guest");
            smb2_set_security_mode(ctx_, SMB2_NEGOTIATE_SIGNING_ENABLED);
        } else {
            smb2_set_user(ctx_, location_.user.c_str());
            smb2_set_password(ctx_, location_.password.c_str());
            smb2_set_domain(ctx_, location_.domain.empty() ? "WORKGROUP"
                                                           : location_.domain.c_str());
        }

        const std::string server = with_port(location_);
        const int status = smb2_connect_share(
            ctx_, server.c_str(), location_.share.c_str(),
            location_.user.empty() ? nullptr : location_.user.c_str());
        if (status < 0) {
            // The resolver's own sentence wins when the host was a name this
            // process cannot resolve: the share was never contacted, so
            // "Invalid address" must not read like a typo in the address and a
            // refused login must not read like a protocol error (P1/P2, BUG 3).
            const std::string resolve_error = take_resolve_error();
            connect_error_ =
                smb_connect_error(location_, server, resolve_error, smb2_get_error(ctx_));
            connect_error_at_ = std::chrono::steady_clock::now();
            error_ = connect_error_;
            set_last_failure(connect_error_);
            drop_locked();
            return false;
        }
        connect_error_.clear();
        error_.clear();
        set_last_failure(std::string());
        return true;
    }

    void disconnect_locked() {
        if (ctx_ == nullptr) return;
        smb2_disconnect_share(ctx_);
        smb2_destroy_context(ctx_);
        ctx_ = nullptr;
    }
    Result<std::vector<uint8_t>> read_locked(const std::string& relative, uint64_t offset,
                                             size_t length) {
        const std::string path = share_path(location_, relative);
        struct smb2fh* handle = smb2_open(ctx_, path.c_str(), O_RDONLY);
        if (handle == nullptr) {
            return Result<std::vector<uint8_t>>::failure(failure("cannot open " + relative));
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(std::min(length, static_cast<size_t>(1) << 26));
        size_t done = 0;
        while (done < length) {
            const size_t chunk = std::min(kChunk, length - done);
            bytes.resize(done + chunk);
            const int got = smb2_pread(ctx_, handle, bytes.data() + done,
                                       static_cast<uint32_t>(chunk), offset + done);
            if (got < 0) {
                (void)smb2_close(ctx_, handle);
                return Result<std::vector<uint8_t>>::failure(failed("cannot read " + relative));
            }
            // A short read is normal in SMB2: the server may cap one response
            // below the requested length without being at the end of the file
            // (impacket's smbserver answers 64 KiB per read), and libsmb2 asks
            // for exactly `chunk` once - it does not continue by itself. Only a
            // zero-length answer ends the read; anything else resumes at
            // offset + done. Treating a short read as EOF truncated a 40 MB
            // scene.psplat to its first 64 KiB (found by the live SMB test).
            if (got == 0) break;
            bytes.resize(done + static_cast<size_t>(got));
            done += static_cast<size_t>(got);
        }
        (void)smb2_close(ctx_, handle);
        bytes.resize(done);
        return Result<std::vector<uint8_t>>::success(std::move(bytes));
    }

    Status list_locked(const std::string& relative, std::vector<DirEntry>* entries) {
        const std::string path = share_path(location_, relative);
        struct smb2dir* dir = smb2_opendir(ctx_, path.c_str());
        if (dir == nullptr) return Status::error(failed("cannot list " + relative));

        for (;;) {
            struct smb2dirent* item = smb2_readdir(ctx_, dir);
            if (item == nullptr) break;
            const std::string name = item->name != nullptr ? item->name : "";
            // Some servers return "." and "..", some do not: skip them when
            // they are there and never pre-read a first entry.
            if (name.empty() || name == "." || name == "..") continue;
            DirEntry entry;
            entry.name = name;
            entry.directory = item->st.smb2_type == SMB2_TYPE_DIRECTORY;
            entry.symlink = item->st.smb2_type == SMB2_TYPE_LINK;
            entry.size = item->st.smb2_size;
            entry.mtime = static_cast<int64_t>(item->st.smb2_mtime);
            entries->push_back(std::move(entry));
        }
        smb2_closedir(ctx_, dir);
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
            if (smb2_unlink(ctx_, share_path(location_, child).c_str()) < 0) {
                return Status::error(failed("cannot remove " + child));
            }
        }
        if (!relative.empty() &&
            smb2_rmdir(ctx_, share_path(location_, relative).c_str()) < 0) {
            return Status::error(failed("cannot remove " + relative));
        }
        return Status::success();
    }

    Location location_;
    mutable std::mutex mutex_;
    struct smb2_context* ctx_ = nullptr;
    std::string error_;          // the last failure, returned by the free functions
    std::string connect_error_;  // a failed connect, held for kConnectRetry
    std::chrono::steady_clock::time_point connect_error_at_{};
    // Why the connection was dropped, "" when up. Guarded by its own mutex, so
    // connection_error() never waits on the adapter's blocking lock.
    mutable std::mutex health_mutex_;
    std::string last_failure_;
};

#endif  // SPLAT_HAVE_SMB2

}  // namespace

bool smb_missing_name(int status) {
    // libsmb2 6.3 reports "no such name" through the return value (-ENOENT,
    // from nterror_to_errno()), and that is the only signal read here:
    // smb2_get_nterror() is not reset per reply, so after one missing-name
    // stat it keeps 0xC0000034 and a later -EACCES answer would read as "no
    // such file" (mac-share/PROBE.md: "prefer the -errno return"). Trusting it
    // made stat() of a denied path look like an absent one, and
    // unique_scene_folder() could then publish over a folder that really
    // existed.
    return status == -ENOENT;
}

std::string smb_url(const Location& location) {
    std::string url = "smb://";
    if (!location.user.empty()) url += location.user + "@";
    url += with_port(location);
    url += "/" + location.share;
    std::string path = location.path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (!path.empty() && path.back() == '/') path.pop_back();
    if (!path.empty()) url += "/" + path;
    if (location.encrypt) url += "?seal";
    return url;
}

std::string smb_connect_host(const Location& location) { return with_port(location); }

std::string smb_connect_error(const Location& location, const std::string& server,
                              const std::string& resolve_error, const char* library_error) {
    // A name this process cannot resolve: the share was never contacted. This
    // is the sentence that turns "Invalid address" into a diagnosis (P1/P2).
    if (!resolve_error.empty()) return location.id + ": " + resolve_error;

    const std::string library =
        library_error != nullptr ? std::string(library_error) : std::string();

    // A refused session setup. libsmb2 reports it as "Session setup failed with
    // (0xc000006d) STATUS_LOGON_FAILURE" against a server whose error reply it
    // can read, and as a rejected reply against one whose it cannot (impacket
    // 0.11.0 writes one byte too many, smb-e2e RESULT.md); both mean the same
    // thing to the user, and neither says so (BUG 3).
    const bool refused =
        library.find("STATUS_LOGON_FAILURE") != std::string::npos ||
        library.find("LOGON_FAILURE") != std::string::npos ||
        library.find("Session setup failed") != std::string::npos ||
        library.find("Failed to parse fixed part of command payload") != std::string::npos ||
        library.find("Error data extends beyond end of PDU") != std::string::npos ||
        // An anonymous (guest) session against a server that requires signing:
        // there is no session key because there was no login to derive one from.
        library.find("Signing required by server") != std::string::npos ||
        library.find("STATUS_ACCOUNT_DISABLED") != std::string::npos ||
        library.find("STATUS_PASSWORD_EXPIRED") != std::string::npos;
    if (refused) {
        std::string message = location.id + ": the server refused the login";
        if (!location.user.empty()) message += " for \"" + location.user + "\"";
        message += ": wrong user name or password";
        message += " (cannot connect to " + server + "/" + location.share + ")";
        return message;
    }

    std::string message = location.id + ": cannot connect to " + server + "/" + location.share;
    if (!library.empty()) message += ": " + library;
    return message;
}

std::shared_ptr<IFileSystem> make_smb_file_system(const Location& location, std::string* error) {
    if (location.host.empty() || location.share.empty()) {
        if (error != nullptr) {
            *error = location.id + ": an SMB location needs a host and a share";
        }
        return nullptr;
    }
#if defined(SPLAT_HAVE_SMB2)
    return std::make_shared<SmbFileSystem>(location);
#else
    if (error != nullptr) {
        *error = location.id + ": SMB support is not built into this version";
    }
    return nullptr;
#endif
}

}  // namespace splat::vfs
