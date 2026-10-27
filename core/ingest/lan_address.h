#pragma once

#include <string>

namespace splat {

// C9. The address the ingest page binds to. It picks an up, non-loopback IPv4
// address from a private range (10/8, 172.16/12, 192.168/16), which is what a
// phone on the same LAN can reach. Link-local and public addresses are never
// chosen, so the page is not exposed by accident.
//
// Returns an empty string when no such interface exists; callers bind loopback
// or fail instead of listening on 0.0.0.0.
std::string detect_lan_address();

}  // namespace splat
