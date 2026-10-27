#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace splat {

// 1536 -> "1.50 KiB". Binary units, two decimals: the one formatter the CLIs
// print byte counts with (converter byte totals, RSS, weight buffers).
inline std::string human_bytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f %s", value, kUnits[unit]);
    return text;
}

}  // namespace splat
