// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/random_bytes.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdlib.h>  // arc4random_buf
#endif

namespace splat {
namespace {

constexpr char kHex[] = "0123456789abcdef";

}  // namespace

void secure_random_bytes(void* out, size_t size) {
    if (size == 0) return;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(out, size);
    return;
#elif defined(__linux__)
    auto* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < size) {
        const ssize_t n = ::getrandom(p + got, size - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (got == size) return;
#endif
    // The portable fallback, and the only source on a platform without a call
    // of its own. A short read is fatal: a partially filled buffer would be
    // predictable where it matters most.
    FILE* file = std::fopen("/dev/urandom", "rb");
    if (file == nullptr) std::abort();
    const size_t read = std::fread(out, 1, size, file);
    std::fclose(file);
    if (read != size) std::abort();
}

std::string random_hex(size_t bytes) {
    std::string out(bytes * 2, '0');
    if (bytes == 0) return out;
    std::string raw(bytes, '\0');
    secure_random_bytes(raw.data(), raw.size());
    for (size_t index = 0; index < bytes; ++index) {
        const unsigned char byte = static_cast<unsigned char>(raw[index]);
        out[2 * index] = kHex[byte >> 4];
        out[2 * index + 1] = kHex[byte & 0x0f];
    }
    return out;
}

}  // namespace splat
