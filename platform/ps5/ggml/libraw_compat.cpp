// One C library function the PS5 payload SDK does not export.
//
// LibRaw's FreeBSD branch calls ::memmem() (src/utils/utils_dcraw.cpp) because
// the target's headers are FreeBSD's, but the payload SDK's libc.a has no
// memmem symbol, so the title link fails with "undefined symbol: memmem".
// Defining it here - in its own translation unit inside libsplat_libraw.a -
// closes the link without touching the library.
//
// This file is compiled into the cross build only: macOS libSystem exports
// memmem, and a second definition there would at best be dead code.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstring>

extern "C" void* memmem(const void* haystack, size_t haystack_length, const void* needle,
                        size_t needle_length) {
    if (needle_length == 0) return const_cast<void*>(haystack);
    if (haystack == nullptr || needle == nullptr) return nullptr;
    if (haystack_length < needle_length) return nullptr;

    const unsigned char* text = static_cast<const unsigned char*>(haystack);
    const unsigned char* pattern = static_cast<const unsigned char*>(needle);
    for (size_t index = 0; index + needle_length <= haystack_length; ++index) {
        if (std::memcmp(text + index, pattern, needle_length) == 0) {
            return const_cast<unsigned char*>(text + index);
        }
    }
    return nullptr;
}
