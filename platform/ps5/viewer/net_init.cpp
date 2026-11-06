// SPDX-License-Identifier: GPL-3.0-or-later

#include "net_init.h"

#include <cstdio>
#include <mutex>
#include <string>

// The one declaration of these exports (see platform/ps5/net/sce_http.h); the
// comment there says why they are not in a SDK header.
#include "platform/ps5/net/resolve_glue.h"
#include "platform/ps5/net/sce_http.h"
#include "vfs/net_state.h"
#include "vfs/resolve.h"

namespace viewer {
namespace {

std::mutex g_mutex;
bool g_started = false;
std::string g_report;

std::string hex_code(int code) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned int>(code));
    return text;
}

}  // namespace

std::string ensure_net_init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Every adapter can ask the stack to come up again after a boot-time
    // failure (L1): the hook is registered once, and it is idempotent.
    splat::vfs::set_net_stack_retry([] { (void)ensure_net_init(); });
    if (g_started) return g_report;

    // sceNetInit returns nonzero when the process already initialised the
    // stack; only a negative value is a failure (ProsperoMgr's rule, and the
    // HTTP fetcher's). The value is logged either way: a network case that
    // fails must say whether the stack came up at all (console pass 3, P2).
    const int init = sceNetInit();
    g_report = "sceNetInit -> " + std::to_string(init);
    if (init < 0) {
        // g_started stays false: a transient failure is retried by the next
        // caller (the mount's reset, or an adapter's connect) instead of being
        // cached for the life of the process. The stack's sentence is what the
        // location's offline line shows, so it never reads as a bad share.
        g_report += " (" + hex_code(init) +
                    "); the console network stack is unavailable, so no location can connect";
        splat::vfs::set_net_stack_error(g_report);
        return g_report;
    }
    splat::vfs::set_net_stack_error(std::string());

    // The resolver, through the same shim the adapters use: a literal must be
    // answered here with no lookup, and the process must say out loud whether a
    // host name can be resolved at all. Half of the console pass's evidence gap
    // was that nothing ever printed either number.
    splat::vfs::NumericAddress self;
    const bool literal = splat::vfs::parse_numeric_host("127.0.0.1", &self);
    g_report += "; resolve 127.0.0.1 -> ";
    g_report += literal ? "0" : "failed";
    if (!splat::vfs::resolver_available(splat::vfs::native_addr_api())) {
        g_report += "; no name service: a location needs the server's IP address";
    } else {
        g_report += "; name service present";
    }
    g_started = true;
    return g_report;
}

}  // namespace viewer
