// The title's half of core/vfs/resolve.cpp: the two resolver entry points.
//
// libsmb2 and libnfs are submodules and both resolve through getaddrinfo()
// (third_party/libsmb2/lib/socket.c, third_party/libnfs/lib/socket.c). This
// file defines getaddrinfo() and freeaddrinfo() for the title, so every
// reference from those archives - and every reference in this executable -
// lands in core/vfs/resolve.cpp instead. The vendored sources are untouched
// (console pass 3, P1).
//
// Why a definition and not the --wrap the pass suggested: this title's link
// already binds getaddrinfo through radv-link.mk's
// `--defsym=getaddrinfo=ps5_getaddrinfo`, and --defsym *is* a definition. With
// both in place lld binds __wrap_getaddrinfo (and getaddrinfo itself) to the
// --defsym target - the SDK's stub - and drops the wrapped definition: measured
// on the built title, __wrap_getaddrinfo and ps5_getaddrinfo were the same
// address and the body there was `movl $4, %eax; ret` (EAI_FAIL). The viewer
// therefore removes those two --defsyms (TITLE_DEFSYM_SKIP in
// platform/ps5/viewer/Makefile) and defines the names here instead.
//
// The SDK's ps5_getaddrinfo is the stub `*res = NULL; return EAI_FAIL`. The
// viewer calls the SDK's stub through its own names (ps5_getaddrinfo), because a
// title PIE may not carry an undefined symbol, weak or not: the converter
// refuses it. A name is therefore probed once through the stub,
// resolver_available() reports "no name service", and the location fails with
// the sentence a user can act on.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/net/resolve_glue.h"

#include <netdb.h>

#include "vfs/resolve.h"

// libps5platform.a's public stubs, the names the platform layer exports. Using
// them directly (rather than through radv-link.mk's --defsym alias) is what
// keeps the link clean: a --defsym alias is only defined when something
// references its target, and a version-script entry for an undefined name is a
// link error.
extern "C" int ps5_getaddrinfo(const char* node, const char* service,
                               const struct addrinfo* hints, struct addrinfo** out);
extern "C" void ps5_freeaddrinfo(struct addrinfo* list);

extern "C" int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                           struct addrinfo** out) {
    return splat::vfs::address_lookup(splat::vfs::native_addr_api(), node, service, hints, out);
}

extern "C" void freeaddrinfo(struct addrinfo* list) {
    splat::vfs::address_release(splat::vfs::native_addr_api(), list);
}

namespace splat::vfs {

AddrInfoApi native_addr_api() {
    AddrInfoApi api;
    api.lookup = &ps5_getaddrinfo;
    api.release = &ps5_freeaddrinfo;
    return api;
}

}  // namespace splat::vfs
