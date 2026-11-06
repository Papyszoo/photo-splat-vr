// Name resolution for the network adapters, split in two halves:
//
//   * a literal ("192.0.2.63", "::1", "[::1]", "::ffff:192.0.2.63") is
//     parsed here and answered without asking anything: no getaddrinfo, no
//     nsswitch, no DNS, no services database. A dotted quad needs no name
//     service, and the console's getaddrinfo returns EAI_FAIL for every input
//     (console pass 3, P1), so this is the only path that can connect there.
//   * a name goes to the process's real resolver - but only when it really has
//     one. The PS5 title's `getaddrinfo` is a stub (the payload SDK's
//     libps5platform.a defines ps5_getaddrinfo as `return EAI_FAIL`), which the
//     first call probes once and caches, so a name fails with "enter the IP
//     address" instead of libsmb2's "Invalid address" (P2).
//
// The logic is here, at the core, so a desktop test can drive exactly what the
// title links: platform/ps5/net/resolve_wrap.cpp is the title's `--wrap` glue
// and tests/resolver_shim.cpp is the same glue for a Mac/Linux test binary
// (ld64 has no --wrap), and both call these functions.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

struct addrinfo;  // <netdb.h> at the call site; this header stays platform-free

namespace splat::vfs {

// One endpoint parsed out of a literal: the address bytes in network order and
// the port, in the shape getaddrinfo() would have produced.
struct NumericAddress {
    int family = 0;  // AF_INET, AF_INET6; 0 when the host is not a literal
    int length = 0;  // 4 or 16 used bytes of `bytes`
    uint8_t bytes[16] = {};
    int port = 0;    // from the service string; 0 when none was given
};

// True when `host` is an IPv4 or IPv6 literal. Accepts the "[...]" form a
// location may carry for an IPv6 address. Never calls a resolver, and never
// guesses: "1.2.3", "999.1.1.1", "010.1.1.1" and "host name" are not literals.
bool parse_numeric_host(const std::string& host, NumericAddress* out);

// The port of a numeric service string, as AI_NUMERICSERV means it: digits
// only, 0..65535. A null or empty service is 0 ("no port") and not an error; a
// name ("https") is refused, because resolving it would need a services
// database. That is the L2 defect: libsmb2 always hands the port over as a
// service string.
bool parse_numeric_service(const char* service, int* port);

// The process's real resolver, supplied by the platform glue (the title
// delegates to __real_getaddrinfo, the test shim to dlsym(RTLD_NEXT)).
struct AddrInfoApi {
    int (*lookup)(const char* node, const char* service, const struct addrinfo* hints,
                  struct addrinfo** out) = nullptr;
    void (*release)(struct addrinfo* list) = nullptr;
};

// The getaddrinfo() decision: a literal is answered from parse_numeric_host(),
// a name is passed to `api` only when resolver_available(api) says this
// process has a name service, and EAI_FAIL otherwise. *out is untouched on
// failure. Addresses this function produced carry a marker and are freed by
// address_release(); anything else goes to the real freeaddrinfo.
int address_lookup(const AddrInfoApi& api, const char* node, const char* service,
                   const struct addrinfo* hints, struct addrinfo** out);
void address_release(const AddrInfoApi& api, struct addrinfo* list);

// True when a name service can answer in this process. Probes the real
// resolver once with "localhost" and caches the answer. SPLAT_RESOLVE_FORCE_FAIL
// (the tests' switch, and how a console is reproduced on the desktop) forces
// false without asking.
bool resolver_available(const AddrInfoApi& api);

// The last name-resolution failure on this thread, as the one sentence the
// Location's offline line shows, or "" when the last lookup did not fail on a
// name. Thread-local: every mount runs on its own thread, and an adapter takes
// it right after the library call that failed.
std::string take_resolve_error();
void clear_resolve_error();

// "cannot resolve \"nas.local\": this build has no name service (EAI_FAIL);
// enter the server's IP address" - and the port variant for L2. Public so the
// tests pin the wording the user reads.
std::string resolve_name_error(const std::string& host);
std::string resolve_service_error(const std::string& service);

}  // namespace splat::vfs
