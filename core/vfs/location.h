// Network locations: what the user configured, and the one file that keeps it.
//
// A Location is one share (SMB) or export (NFS) plus the sub-path the library
// lives in. Its `id` is the string a VFS path starts with (`smb0:/photos`); it
// is assigned once and never reused, so a path saved in ui.json or in a job
// keeps pointing at the same place after another location is removed and
// added. The store lives at `<state>/locations.json` (core/ui/services.h's
// UiPaths::state), mode 0600, because it holds a plain-text password: see
// docs/network-locations.md, which says so on the screen too.
//
// A missing file is an empty store; a corrupt one is an error naming the file
// (an operator can inspect it), never a silent reset.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "vfs/io.h"

namespace splat::vfs {

enum class LocationProtocol { smb, nfs };

const char* location_protocol_name(LocationProtocol protocol);

struct Location {
    std::string id;      // "smb0"; assigned by LocationStore::add, never changed
    std::string name;    // display name; empty = host/share
    LocationProtocol protocol = LocationProtocol::smb;
    std::string host;    // "nas.local", "192.168.1.20", "::1"
    int port = 0;        // 0 = the protocol default (445 / 2049)
    std::string share;   // SMB share name, NFS export path
    std::string path;    // sub-path inside the share ("" = its root)
    std::string user;    // "" = guest (SMB) / AUTH_SYS (NFS)
    std::string password;  // plain text in the file; the screen warns about it
    std::string domain = "WORKGROUP";
    bool encrypt = false;        // SMB: smb2_set_seal (AES-128-CCM)
    bool photo_source = false;   // show a Photos tab for it
    bool splat_library = false;  // offer it as a library root
    // NFS only (L4); kept here so one file describes both protocols. AUTH_SYS
    // is the client's uid/gid and nothing else, so 65534 (nobody/nogroup) is
    // the default: a console that names no user must not be root on the NAS.
    int nfs_version = 3;  // 3 (default) or 4
    uint32_t uid = 65534;
    uint32_t gid = 65534;

    // The name the locations screen shows: `name`, else host/share.
    std::string display_name() const;
    // The port to connect to: `port`, else the protocol default.
    int effective_port() const;
};

// Parses the JSON document. Exposed for the tests; the store is the normal
// entry point.
Result<std::vector<Location>> locations_from_json(const std::string& json);
std::string locations_to_json(const std::vector<Location>& locations);

class LocationStore {
public:
    // The largest locations.json this build will read, and the most entries it
    // may hold. The file is on the boot path and this app is its only writer: a
    // bigger one is not a store it produced. Both are checked before the bytes
    // are read or a Location is built (the 0.5.0 crash was a state file whose
    // count drove an allocation; REPORT-CR.md).
    static constexpr std::size_t kMaxFileBytes = 256u << 10;
    static constexpr std::size_t kMaxEntries = 256;

    // Reads `file` when it exists, otherwise starts empty. A malformed file is
    // an error that names it.
    static Result<LocationStore> open(const Path& file);

    const Path& file() const { return file_; }
    const std::vector<Location>& locations() const { return locations_; }
    const Location* find(const std::string& id) const;

    // Assigns the next stable id for the protocol ("smb0", "smb1", ..., then
    // nfs) and saves. The result is the id.
    Result<std::string> add(Location location);
    // Replaces the location with the same id. Fails for an unknown id.
    Status update(const Location& location);
    // Drops the location and saves. Fails for an unknown id.
    Status remove(const std::string& id);
    Status save() const;

    // Mounts every location into `file_systems` and returns one line per
    // location: "<id>: ready" or "<id>: offline: <reason>". A location that
    // fails stays in the store; its adapter is simply not registered.
    std::vector<std::string> mount_all(FileSystems& file_systems);

    // Builds and registers one adapter per location *without* probing it, and
    // returns the ids that could not even be built ("<id>: <reason>"). An
    // adapter connects lazily, so this cannot block, and every location path
    // (`smb0:/scenes`) resolves by id from the moment the store is open. The
    // job queue starts before the mount worker has answered, and a job resumed
    // at boot stage-and-uploads into its destination; without this pass it
    // failed with "unknown location \"smb0\"" although the location was
    // configured in the same boot (the console pass's 09:24 run).
    std::vector<std::string> register_adapters(FileSystems& file_systems) const;

    // The one mount implementation: builds the adapter, proves it with a root
    // stat, registers it, and returns the state the Locations screen shows -
    // "ready" or "offline: <reason>" (the reason names the id). mount_all() and
    // the UI's mount worker both call this, so the two cannot drift apart.
    // A location whose adapter register_adapters() already added is probed
    // through that same adapter, not through a second one.
    static std::string mount_one(FileSystems& file_systems, const Location& location);

private:
    Path file_;
    std::vector<Location> locations_;
    int next_smb_ = 0;
    int next_nfs_ = 0;
    void recount_ids();
};

}  // namespace splat::vfs
