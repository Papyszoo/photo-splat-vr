// The NFS adapter: one IFileSystem per mounted export, over libnfs 8.0.0.
//
// libnfs is blocking and single-threaded, so the context is created lazily on
// the first read/listing under the adapter's mutex and every call happens on a
// worker thread (AsyncListing, the thumbnail cache, the job runner), exactly
// like core/vfs/smb.cpp. The UI thread never reaches this file.
//
// There is no password: NFS authenticates with AUTH_SYS, which is the client's
// uid/gid and nothing else. That is a property of the protocol, not a limit of
// the library, and it is why the edit screen hides User/Password for NFS.
//
// Errors name the location ("nfs0: ...") and carry libnfs's own message; the
// URL below never carries a credential because there is none to carry.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>

#include "vfs/io.h"
#include "vfs/location.h"

namespace splat::vfs {

// `nfs://<host>/<export>[/<path>]?version=3&nfsport=<p>&mountport=<p>&uid=<u>&gid=<g>`
// for a log line or the locations screen. `version=4` drops `mountport` (there
// is no MOUNT protocol in NFSv4), and the two port arguments are left out when
// the location uses the protocol defaults, so a NAS reached through rpcbind
// keeps working. libnfs accepts exactly these query arguments.
std::string nfs_url(const Location& location);

// A mounted export, or nullptr with `error` filled. The connection itself is
// made on the first operation (lazy), so a mount of a switched-off NAS still
// returns an adapter and its first listing says "offline". A location with no
// host or export fails here with a message naming the id.
std::shared_ptr<IFileSystem> make_nfs_file_system(const Location& location, std::string* error);

}  // namespace splat::vfs
