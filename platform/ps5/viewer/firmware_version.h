// The text form of kern.sdk_version, shared by the viewer's start line
// (main.cpp) and the unsupported-firmware card (firmware_message.h): one
// decoder, so the card and the klog cannot disagree.
//
// The console reports the version as BCD digits per byte, not as decimal:
// 10.01 is 0x10010000, 13.60 is 0x13600007, 7.61 is 0x07610001. The top byte
// is the major ("10", "13", "7") and the next byte the zero-padded minor
// ("01", "60", "61"); the low 16 bits (the build) are not part of the name.
// This is the viewer's long-standing "%x.%02x" start line (main.cpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace viewer {

// "10.01" from kern.sdk_version 0x10010000; empty when the version is unknown,
// which the card then words without a number.
inline std::string format_firmware_version(uint32_t sdk_version) {
    if (sdk_version == 0) return std::string();
    char text[16];
    std::snprintf(text, sizeof(text), "%x.%02x", static_cast<unsigned>(sdk_version >> 24),
                  static_cast<unsigned>((sdk_version >> 16) & 0xffu));
    return text;
}

}  // namespace viewer
