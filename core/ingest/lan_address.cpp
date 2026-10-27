#include "ingest/lan_address.h"

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace splat {
namespace {

bool is_private_ipv4(uint32_t address) {
    // Addresses are in network byte order; compare byte by byte.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address);
    if (bytes[0] == 10) return true;
    if (bytes[0] == 192 && bytes[1] == 168) return true;
    if (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) return true;
    return false;
}

std::string address_to_string(uint32_t address) {
    char text[INET_ADDRSTRLEN] = {};
    const auto* bytes = reinterpret_cast<const unsigned char*>(&address);
    if (::inet_ntop(AF_INET, bytes, text, sizeof(text)) == nullptr) return {};
    return text;
}

}  // namespace

std::string detect_lan_address() {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
    struct ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) return {};

    std::string found;
    for (struct ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr) continue;
        if (entry->ifa_addr->sa_family != AF_INET) continue;
        if ((entry->ifa_flags & IFF_UP) == 0) continue;
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0) continue;

        const auto* address = reinterpret_cast<const struct sockaddr_in*>(entry->ifa_addr);
        if (!is_private_ipv4(address->sin_addr.s_addr)) continue;

        found = address_to_string(address->sin_addr.s_addr);
        if (!found.empty()) break;
    }

    ::freeifaddrs(interfaces);
    return found;
#else
    return {};
#endif
}

}  // namespace splat
