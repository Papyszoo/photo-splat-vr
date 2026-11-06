// The desktop test's half of the title's resolver shim.
//
// The title defines getaddrinfo()/freeaddrinfo() in
// platform/ps5/net/resolve_glue.cpp, which binds every reference from the static
// archives linked into it - libsmb2 and libnfs included. The same two symbols
// are defined here the same way, so the live SMB/NFS cases exercise the title's
// path and not the Mac's working resolver. The decision logic is
// core/vfs/resolve.cpp, the same file the title runs; only the delegate differs
// (the SDK stub on the console, dlsym's next image here).
//
// Names still resolve normally on the desktop (the real getaddrinfo answers);
// SPLAT_RESOLVE_FORCE_FAIL=1 makes them fail, which is what the console looks
// like and what the [smb-live]/[nfs-live] cases use.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#if !defined(_WIN32)

#include <dlfcn.h>
#include <netdb.h>

#include "vfs/resolve.h"

namespace {

using LookupFn = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
using ReleaseFn = void (*)(struct addrinfo*);

LookupFn real_lookup() {
    static LookupFn function =
        reinterpret_cast<LookupFn>(dlsym(RTLD_NEXT, "getaddrinfo"));
    return function;
}

ReleaseFn real_release() {
    static ReleaseFn function =
        reinterpret_cast<ReleaseFn>(dlsym(RTLD_NEXT, "freeaddrinfo"));
    return function;
}

splat::vfs::AddrInfoApi real_api() {
    splat::vfs::AddrInfoApi api;
    api.lookup = real_lookup();
    api.release = real_release();
    return api;
}

}  // namespace

extern "C" int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                           struct addrinfo** out) {
    return splat::vfs::address_lookup(real_api(), node, service, hints, out);
}

extern "C" void freeaddrinfo(struct addrinfo* list) {
    splat::vfs::address_release(real_api(), list);
}

#endif  // !_WIN32
