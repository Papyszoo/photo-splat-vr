// One sceNetInit() for every network location mount.
//
// libsmb2 (and later libnfs) call socket/connect/poll/getaddrinfo directly,
// which a PS5 title may only do after the network stack is up. The declaration
// lives in platform/ps5/net/sce_http.h (it is the same export the HTTP fetcher
// uses); this is only the once-per-process call, kept out of core/ because a
// console service must not appear in the desktop build.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace viewer {

// Starts the console network stack on the first call and does nothing
// afterwards. Returns an empty string on success, otherwise a line for klog
// ("sceNetInit failed (...)").
std::string ensure_net_init();

}  // namespace viewer
