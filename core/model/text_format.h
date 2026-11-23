// The small text helpers the model code shares: one suffix test and the byte /
// rate / duration numbers the converter, the streaming converter and the
// manager print in their klog lines. One copy each, so the three paths report
// the same sizes in the same shape (the first console klogs compared them by
// eye and by script).
//
// Before this header every copy was local to its file: ends_with in
// converter.cpp and stream_converter.cpp (and da2_profile.cpp), gigabyte_text /
// rate_text / seconds_text in converter.cpp, stream_converter.cpp and
// model_manager.cpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace splat {

inline bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "2.81 GB": bytes / 1e9, two decimals (disk sizes, as the console's UI says).
inline std::string gigabyte_text(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

// "1.31 MB": bytes / 1e6, one decimal (transfer sizes).
inline std::string megabyte_text(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

// "93.7 MB/s": bytes / 1e6 over the seconds it took. With no elapsed time there
// is no rate to report.
inline std::string rate_text(uint64_t bytes, double seconds) {
    char text[32];
    if (seconds <= 0.0) return "? MB/s";
    std::snprintf(text, sizeof(text), "%.1f MB/s", static_cast<double>(bytes) / seconds / 1e6);
    return text;
}

// "3.7": a duration in seconds, one decimal.
inline std::string seconds_text(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", seconds);
    return text;
}

}  // namespace splat
