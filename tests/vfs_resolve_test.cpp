// core/vfs/resolve: the numeric path that has to work inside the PS5 title.
//
// Console pass 3 (P1) found getaddrinfo returning EAI_FAIL for *every* input,
// a dotted quad included, so no SMB or NFS location ever mounted. These cases
// pin the replacement: a literal is answered from the bytes in the string, with
// no resolver call at all, and a name fails with the reason a user can act on
// when the process has no name service (P2). The counting api below is how "no
// resolver call" is asserted rather than assumed.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <netdb.h>
#include <netinet/in.h>

#include "vfs/resolve.h"

namespace vfs = splat::vfs;

namespace {

// The resolver every case here installs: it counts its calls and answers
// EAI_FAIL. A literal must never reach it.
struct NoResolver {
    int calls = 0;
    splat::vfs::AddrInfoApi api;

    NoResolver() {
        api.lookup = [](const char*, const char*, const struct addrinfo*, struct addrinfo** out) {
            ++instance()->calls;
            *out = nullptr;
            return EAI_FAIL;
        };
        api.release = nullptr;
        instance() = this;
    }
    ~NoResolver() { instance() = nullptr; }

    static NoResolver*& instance() {
        static NoResolver* current = nullptr;
        return current;
    }
};

struct Sockaddr {
    int family = 0;
    int port = 0;
    std::string address;  // a dotted quad, or the first 8 IPv6 bytes as hex
    std::size_t length = 0;
};

Sockaddr describe(const struct addrinfo* list) {
    Sockaddr out;
    if (list == nullptr) return out;
    out.family = list->ai_family;
    out.length = list->ai_addrlen;
    if (list->ai_family == AF_INET) {
        const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(list->ai_addr);
        out.port = ntohs(in->sin_port);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&in->sin_addr);
        out.address = std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
                      std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    } else {
        const sockaddr_in6* in6 = reinterpret_cast<const sockaddr_in6*>(list->ai_addr);
        out.port = ntohs(in6->sin6_port);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&in6->sin6_addr);
        char text[40];
        std::snprintf(text, sizeof(text),
                      "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                      bytes[14], bytes[15]);
        out.address = text;
    }
    return out;
}

}  // namespace

TEST_CASE("a literal host is parsed here: IPv4, IPv6, the brackets and the mapped form") {
    vfs::NumericAddress address;

    REQUIRE(vfs::parse_numeric_host("192.0.2.63", &address));
    CHECK(address.family == AF_INET);
    CHECK(address.length == 4);
    CHECK(address.bytes[0] == 192);
    CHECK(address.bytes[1] == 0);
    CHECK(address.bytes[2] == 2);
    CHECK(address.bytes[3] == 63);

    REQUIRE(vfs::parse_numeric_host("127.0.0.1", &address));
    CHECK(address.bytes[0] == 127);
    CHECK(address.bytes[3] == 1);

    REQUIRE(vfs::parse_numeric_host("::1", &address));
    CHECK(address.family == AF_INET6);
    CHECK(address.length == 16);
    for (int i = 0; i < 15; ++i) CHECK(address.bytes[i] == 0);
    CHECK(address.bytes[15] == 1);

    // libsmb2 brackets an IPv6 server string; the brackets are not the address.
    REQUIRE(vfs::parse_numeric_host("[::1]", &address));
    CHECK(address.family == AF_INET6);
    CHECK(address.bytes[15] == 1);

    // ::ffff:192.0.2.63 - the form an IPv4-mapped socket prints.
    REQUIRE(vfs::parse_numeric_host("::ffff:192.0.2.63", &address));
    CHECK(address.family == AF_INET6);
    CHECK(address.bytes[10] == 0xff);
    CHECK(address.bytes[11] == 0xff);
    CHECK(address.bytes[12] == 192);
    CHECK(address.bytes[13] == 0);
    CHECK(address.bytes[14] == 2);
    CHECK(address.bytes[15] == 63);

    REQUIRE(vfs::parse_numeric_host("fe80::1", &address));
    CHECK(address.bytes[0] == 0xfe);
    CHECK(address.bytes[1] == 0x80);
    CHECK(address.bytes[15] == 1);

    REQUIRE(vfs::parse_numeric_host("::", &address));
    for (int i = 0; i < 16; ++i) CHECK(address.bytes[i] == 0);
}

TEST_CASE("a non-literal host is never guessed at") {
    // Every one of these is a name to this code. "" and " " are refused rather
    // than read as 0.0.0.0; "1.2.3" and "999.1.1.1" are not addresses; "010"
    // would be octal to a reader and invalid to inet_pton.
    for (const char* text : {"", " ", "1.2.3", "1.2.3.4.5", "999.1.1.1", "010.1.1.1",
                             "192.0.2.63 ", "host name", "nas.local", ":::1", "1::2::3",
                             "12345::", "::1:", "::ffff:999.1.1.1", "192.168.1"}) {
        vfs::NumericAddress address;
        CHECK_MESSAGE(!vfs::parse_numeric_host(text, &address), "read as a literal: " << text);
        CHECK(address.family == 0);
    }
}

TEST_CASE("the port is a number, as AI_NUMERICSERV demands") {
    int port = -1;
    for (const char* text : {"0", "1", "445", "1445", "2049", "65535"}) {
        CHECK_MESSAGE(vfs::parse_numeric_service(text, &port), text);
    }
    CHECK(vfs::parse_numeric_service("1445", &port));
    CHECK(port == 1445);
    // libnfs passes no service at all; that is "the port the caller sets later".
    CHECK(vfs::parse_numeric_service(nullptr, &port));
    CHECK(port == 0);
    CHECK(vfs::parse_numeric_service("", &port));
    CHECK(port == 0);

    // A services-database name would need /etc/services (the L2 defect); a
    // negative or oversized number is not a port.
    for (const char* text : {"65536", "-1", "+1", "4x", " 1445", "1445 ", "https", "0x10"}) {
        CHECK_MESSAGE(!vfs::parse_numeric_service(text, &port), "accepted as a port: " << text);
    }
}

TEST_CASE("an address lookup of a literal never calls a resolver") {
    NoResolver counter;

    struct Case {
        const char* host;
        const char* service;
        int family;
        int port;
        const char* address;
    };
    const Case cases[] = {
        {"192.0.2.63", "445", AF_INET, 445, "192.0.2.63"},
        {"192.0.2.63", "1445", AF_INET, 1445, "192.0.2.63"},
        {"127.0.0.1", "1", AF_INET, 1, "127.0.0.1"},
        {"127.0.0.1", "65535", AF_INET, 65535, "127.0.0.1"},
        {"127.0.0.1", nullptr, AF_INET, 0, "127.0.0.1"},
        {"127.0.0.1", "0", AF_INET, 0, "127.0.0.1"},
        {"[::1]", "1445", AF_INET6, 1445, "00000000000000000000000000000001"},
        {"::1", nullptr, AF_INET6, 0, "00000000000000000000000000000001"},
        {"::ffff:192.0.2.63", "445", AF_INET6, 445,
         "00000000000000000000ffffc000023f"},
    };
    for (const Case& one : cases) {
        struct addrinfo* list = nullptr;
        const int result = vfs::address_lookup(counter.api, one.host, one.service, nullptr, &list);
        CHECK_MESSAGE(result == 0, one.host);
        REQUIRE(list != nullptr);
        const Sockaddr described = describe(list);
        CHECK(described.family == one.family);
        CHECK(described.port == one.port);
        CHECK(described.address == one.address);
        CHECK(described.length ==
              (one.family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)));
        // What libsmb2's connect_async_ai reads: a stream socket.
        CHECK(list->ai_socktype == SOCK_STREAM);
        // Nothing was asked, so nothing could have failed.
        CHECK(vfs::take_resolve_error().empty());

        vfs::address_release(counter.api, list);
    }
    CHECK(counter.calls == 0);
}

TEST_CASE("a literal is answered even when the process has no resolver at all") {
    // The console's process, exactly: every name fails, so the only way in is
    // an address. This is the case P1 was about.
    setenv("SPLAT_RESOLVE_FORCE_FAIL", "1", 1);
    NoResolver counter;
    struct addrinfo* list = nullptr;
    const int literal = vfs::address_lookup(counter.api, "192.0.2.63", "1445", nullptr, &list);
    CHECK(literal == 0);
    REQUIRE(list != nullptr);
    CHECK(describe(list).address == "192.0.2.63");
    vfs::address_release(counter.api, list);
    CHECK(counter.calls == 0);

    // A name fails with the sentence a user can act on, and names the code.
    list = nullptr;
    const int named = vfs::address_lookup(counter.api, "nas.local", "1445", nullptr, &list);
    CHECK(named == EAI_FAIL);
    CHECK(list == nullptr);
    const std::string error = vfs::take_resolve_error();
    CHECK(error.find("nas.local") != std::string::npos);
    CHECK(error.find("no name service") != std::string::npos);
    CHECK(error.find("EAI_FAIL") != std::string::npos);
    CHECK(error.find("enter the server's IP address") != std::string::npos);

    // A taken error is gone: the next lookup starts clean.
    CHECK(vfs::take_resolve_error().empty());
    CHECK(counter.calls == 0);
    unsetenv("SPLAT_RESOLVE_FORCE_FAIL");
}

TEST_CASE("a non-numeric service is refused before any connect, with EAI_SERVICE") {
    NoResolver counter;
    struct addrinfo* list = nullptr;
    const int result = vfs::address_lookup(counter.api, "192.0.2.63", "smb", nullptr, &list);
    CHECK(result == EAI_SERVICE);
    CHECK(list == nullptr);
    const std::string error = vfs::take_resolve_error();
    CHECK(error.find("smb") != std::string::npos);
    CHECK(error.find("not a number") != std::string::npos);
    CHECK(counter.calls == 0);
}

TEST_CASE("the hints' family is honoured, and a mismatch is EAI_FAMILY") {
    NoResolver counter;
    struct addrinfo hints {};
    hints.ai_family = AF_INET6;
    struct addrinfo* list = nullptr;
    CHECK(vfs::address_lookup(counter.api, "192.0.2.63", "445", &hints, &list) == EAI_FAMILY);
    CHECK(list == nullptr);

    hints.ai_family = AF_INET;
    CHECK(vfs::address_lookup(counter.api, "192.0.2.63", "445", &hints, &list) == 0);
    REQUIRE(list != nullptr);
    CHECK(list->ai_family == AF_INET);
    vfs::address_release(counter.api, list);

    hints.ai_family = AF_UNSPEC;
    CHECK(vfs::address_lookup(counter.api, "::1", "445", &hints, &list) == 0);
    REQUIRE(list != nullptr);
    CHECK(list->ai_family == AF_INET6);
    vfs::address_release(counter.api, list);
    CHECK(counter.calls == 0);
}

TEST_CASE("release is a no-op for a null list and a plain free for ours") {
    splat::vfs::AddrInfoApi api;  // no hooks at all
    vfs::address_release(api, nullptr);
    {
        NoResolver counter;
        struct addrinfo* list = nullptr;
        REQUIRE(vfs::address_lookup(counter.api, "127.0.0.1", "445", nullptr, &list) == 0);
        REQUIRE(list != nullptr);
        // Releasing our own list must not reach the real freeaddrinfo: this api
        // has none, so a delegation would be a null call.
        vfs::address_release(api, list);
    }
    CHECK(true);
}
