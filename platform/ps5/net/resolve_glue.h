// The process's real resolver, for the title's getaddrinfo()/freeaddrinfo()
// definitions (platform/ps5/net/resolve_glue.cpp) and the one net-init log line.
//
// core/vfs/resolve.h owns the decision logic; this header is only the platform
// name of the resolver it delegates to - which on this console is no resolver at
// all (the SDK's ps5_getaddrinfo is a stub returning EAI_FAIL), so the api is
// empty and a host name fails with the sentence a user can act on. net_init.cpp
// asks resolver_available() so the first klog line of a sitting says so (console
// pass 3, P2: six mount failures blamed a share that was never contacted).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vfs/resolve.h"

namespace splat::vfs {

// The platform's real resolver: {nullptr, nullptr} when it has none.
AddrInfoApi native_addr_api();

}  // namespace splat::vfs
