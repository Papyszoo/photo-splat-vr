// SPDX-License-Identifier: GPL-3.0-or-later

#include "vfs/resolve.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <arpa/inet.h>  // htons
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace splat::vfs {
namespace {

// The marker a list this file allocated carries in ai_flags. Nothing else in
// this process produces an addrinfo whose flags are these four bytes, so
// address_release() can tell its own lists from the real resolver's without a
// registry (and without reading a pointer it does not own).
constexpr int kNumericListTag = 0x53504C54;  // "SPLT"

struct NumericNode {
    struct addrinfo ai;
    struct sockaddr_storage address;
};

bool all_digits(const char* text, std::size_t* length) {
    if (text == nullptr) return false;
    std::size_t count = 0;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') return false;
        ++count;
    }
    *length = count;
    return count > 0;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Four decimal fields, 0..255, separated by exactly three dots. A field with a
// leading zero is not a literal: "010.1.1.1" means different things to
// inet_pton (invalid) and to a reader (octal), so it is left to the name path.
bool parse_ipv4(const std::string& text, uint8_t out[4]) {
    std::size_t start = 0;
    for (int field = 0; field < 4; ++field) {
        const std::size_t dot = text.find('.', start);
        const bool last = field == 3;
        if (last != (dot == std::string::npos)) return false;
        const std::string piece =
            text.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (piece.empty() || piece.size() > 3) return false;
        if (piece.size() > 1 && piece[0] == '0') return false;
        int value = 0;
        for (const char c : piece) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        if (value > 255) return false;
        out[field] = static_cast<uint8_t>(value);
        if (!last) start = dot + 1;
    }
    return true;
}

// The colon-separated 16-bit groups of an IPv6 literal, with at most one "::".
// `head`/`tail` are the parts before and after it; a trailing dotted quad in
// the last group counts as the last two groups (::ffff:192.0.2.63).
bool parse_ipv6_groups(const std::string& text, uint16_t* groups, int* count) {
    int used = 0;
    if (text.empty()) {
        *count = 0;
        return true;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t colon = text.find(':', start);
        const std::string piece =
            text.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (piece.empty()) return false;
        if (piece.find('.') != std::string::npos) {
            // Only the last group may be a dotted quad.
            if (colon != std::string::npos) return false;
            uint8_t quad[4];
            if (!parse_ipv4(piece, quad)) return false;
            if (used + 2 > 8) return false;
            groups[used++] = static_cast<uint16_t>((quad[0] << 8) | quad[1]);
            groups[used++] = static_cast<uint16_t>((quad[2] << 8) | quad[3]);
        } else {
            if (piece.size() > 4) return false;
            int value = 0;
            for (const char c : piece) {
                const int digit = hex_digit(c);
                if (digit < 0) return false;
                value = value * 16 + digit;
            }
            if (used >= 8) return false;
            groups[used++] = static_cast<uint16_t>(value);
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    *count = used;
    return true;
}

bool parse_ipv6(const std::string& text, uint8_t out[16]) {
    if (text.empty()) return false;
    const std::size_t gap = text.find("::");
    if (gap != std::string::npos && text.find("::", gap + 2) != std::string::npos) return false;

    uint16_t head[8] = {0};
    uint16_t tail[8] = {0};
    int head_count = 0;
    int tail_count = 0;
    if (gap == std::string::npos) {
        if (!parse_ipv6_groups(text, head, &head_count)) return false;
        if (head_count != 8) return false;  // no gap: every group must be there
    } else {
        const std::string before = text.substr(0, gap);
        const std::string after = text.substr(gap + 2);
        if (!parse_ipv6_groups(before, head, &head_count)) return false;
        if (!parse_ipv6_groups(after, tail, &tail_count)) return false;
        // "::" stands for at least one zero group.
        if (head_count + tail_count > 7) return false;
    }

    std::memset(out, 0, 16);
    for (int i = 0; i < head_count; ++i) {
        out[i * 2] = static_cast<uint8_t>(head[i] >> 8);
        out[i * 2 + 1] = static_cast<uint8_t>(head[i] & 0xFF);
    }
    const int tail_start = 8 - tail_count;
    for (int i = 0; i < tail_count; ++i) {
        out[(tail_start + i) * 2] = static_cast<uint8_t>(tail[i] >> 8);
        out[(tail_start + i) * 2 + 1] = static_cast<uint8_t>(tail[i] & 0xFF);
    }
    return true;
}

// One node, address included, so address_release() is one free per node. The
// list has a single entry: a literal is exactly one address.
struct addrinfo* make_numeric_node(const NumericAddress& address, const struct addrinfo* hints) {
    NumericNode* node = static_cast<NumericNode*>(std::calloc(1, sizeof(NumericNode)));
    if (node == nullptr) return nullptr;
    node->ai.ai_flags = kNumericListTag;
    node->ai.ai_family = address.family;
    node->ai.ai_socktype = hints != nullptr && hints->ai_socktype != 0 ? hints->ai_socktype
                                                                       : SOCK_STREAM;
    node->ai.ai_protocol = hints != nullptr && hints->ai_protocol != 0 ? hints->ai_protocol
                                                                       : IPPROTO_TCP;
    if (address.family == AF_INET) {
        sockaddr_in* in = reinterpret_cast<sockaddr_in*>(&node->address);
        in->sin_family = AF_INET;
        in->sin_port = htons(static_cast<uint16_t>(address.port));
        std::memcpy(&in->sin_addr, address.bytes, 4);
        node->ai.ai_addr = reinterpret_cast<sockaddr*>(in);
        node->ai.ai_addrlen = sizeof(sockaddr_in);
    } else {
        sockaddr_in6* in6 = reinterpret_cast<sockaddr_in6*>(&node->address);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(static_cast<uint16_t>(address.port));
        std::memcpy(&in6->sin6_addr, address.bytes, 16);
        node->ai.ai_addr = reinterpret_cast<sockaddr*>(in6);
        node->ai.ai_addrlen = sizeof(sockaddr_in6);
    }
    return &node->ai;
}

thread_local std::string tls_resolve_error;

std::string error_text(const std::string& text) {
    tls_resolve_error = text;
    return text;
}

}  // namespace

bool parse_numeric_host(const std::string& host, NumericAddress* out) {
    if (out == nullptr) return false;
    *out = NumericAddress{};

    std::string text = host;
    // libsmb2 brackets an IPv6 literal so the ":port" suffix still splits; the
    // brackets are not part of the address.
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.empty()) return false;

    if (text.find(':') != std::string::npos) {
        if (!parse_ipv6(text, out->bytes)) return false;
        out->family = AF_INET6;
        out->length = 16;
        return true;
    }
    if (!parse_ipv4(text, out->bytes)) return false;
    out->family = AF_INET;
    out->length = 4;
    return true;
}

bool parse_numeric_service(const char* service, int* port) {
    if (port == nullptr) return false;
    if (service == nullptr || service[0] == '\0') {
        *port = 0;
        return true;
    }
    std::size_t length = 0;
    if (!all_digits(service, &length)) return false;
    if (length > 5) return false;
    int value = 0;
    for (const char* c = service; *c != '\0'; ++c) {
        value = value * 10 + (*c - '0');
        if (value > 65535) return false;
    }
    *port = value;
    return true;
}

std::string resolve_name_error(const std::string& host) {
    // The symbolic name, never the number: EAI_FAIL is 4 on macOS/FreeBSD and
    // -4 on glibc, and the user sentence must read the same on the desktop
    // build and on the console (the repo's CI runs this suite on Linux).
    return "cannot resolve \"" + host +
           "\": this build has no name service (EAI_FAIL); enter the server's IP address";
}

std::string resolve_service_error(const std::string& service) {
    return "the port \"" + service +
           "\" is not a number; enter it as a port (1-65535) instead";
}

bool resolver_available(const AddrInfoApi& api) {
    // The switch is checked first and never cached: a test turns it on and off
    // inside one process, and a console must never see a cached "yes".
    const char* forced = std::getenv("SPLAT_RESOLVE_FORCE_FAIL");
    if (forced != nullptr && forced[0] != '\0' && std::strcmp(forced, "0") != 0) return false;
    if (api.lookup == nullptr) return false;

    static std::mutex mutex;
    static int cached = -1;  // -1 unknown, 0 no, 1 yes
    std::lock_guard<std::mutex> lock(mutex);
    if (cached >= 0) return cached == 1;

    struct addrinfo* probe = nullptr;
    const int result = api.lookup("localhost", nullptr, nullptr, &probe);
    const bool ok = result == 0 && probe != nullptr;
    if (probe != nullptr && api.release != nullptr) api.release(probe);
    cached = ok ? 1 : 0;
    return ok;
}

int address_lookup(const AddrInfoApi& api, const char* node, const char* service,
                   const struct addrinfo* hints, struct addrinfo** out) {
    if (out == nullptr) return EAI_FAIL;
    // Failures are reported once, by the adapter that takes them; a fresh
    // attempt starts clean.
    tls_resolve_error.clear();

    if (node == nullptr || node[0] == '\0') {
        // No name at all (AI_PASSIVE): nothing to parse, and the real resolver
        // is the only thing that can answer it.
        if (resolver_available(api)) return api.lookup(node, service, hints, out);
        error_text(resolve_name_error(""));
        return EAI_NONAME;
    }

    NumericAddress literal;
    if (parse_numeric_host(node, &literal)) {
        int port = 0;
        if (!parse_numeric_service(service, &port)) {
            error_text(resolve_service_error(service == nullptr ? std::string() : service));
            return EAI_SERVICE;
        }
        literal.port = port;
        if (hints != nullptr && hints->ai_family != AF_UNSPEC && hints->ai_family != literal.family) {
            return EAI_FAMILY;
        }
        struct addrinfo* list = make_numeric_node(literal, hints);
        if (list == nullptr) return EAI_MEMORY;
        *out = list;
        return 0;
    }

    if (resolver_available(api)) return api.lookup(node, service, hints, out);
    error_text(resolve_name_error(node));
    return EAI_FAIL;
}

void address_release(const AddrInfoApi& api, struct addrinfo* list) {
    if (list != nullptr && list->ai_flags == kNumericListTag) {
        std::free(list);
        return;
    }
    if (list != nullptr && api.release != nullptr) api.release(list);
}

std::string take_resolve_error() {
    std::string text;
    text.swap(tls_resolve_error);
    return text;
}

void clear_resolve_error() { tls_resolve_error.clear(); }

}  // namespace splat::vfs
