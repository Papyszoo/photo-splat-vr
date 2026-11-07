// The Models row's time estimate for a download (0.6.0): "about 5 min left" from
// the real byte rate, smoothed.
//
// Header-only so the tests need no fixture. The rate is an exponential average
// over the progress samples (time constant kRateSeconds), which follows a
// connection that speeds up or slows down without jumping on every report.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace splat {

class DownloadRate {
public:
    // Seconds of the average: a change in speed is mostly followed after this long.
    static constexpr double kRateSeconds = 8.0;
    // Samples closer than this are folded into the next one.
    static constexpr double kMinWindowSeconds = 1.0;
    // No estimate until this much time has been observed.
    static constexpr double kWarmupSeconds = 3.0;

    void reset() { *this = DownloadRate(); }

    // `bytes` is the total received so far (resumed bytes included), `seconds` a
    // monotonic clock. The first sample is only the starting point: a resumed
    // download does not count the bytes it already had as transferred.
    void add(double seconds, uint64_t bytes) {
        if (!started_) {
            started_ = true;
            first_seconds_ = last_seconds_ = seconds;
            last_bytes_ = bytes;
            return;
        }
        const double window = seconds - last_seconds_;
        if (window < kMinWindowSeconds) return;
        const double instant = bytes > last_bytes_ ? static_cast<double>(bytes - last_bytes_) / window : 0.0;
        if (!has_rate_) {
            rate_ = instant;
            has_rate_ = true;
        } else {
            const double weight = 1.0 - std::exp(-window / kRateSeconds);
            rate_ += (instant - rate_) * weight;
        }
        last_seconds_ = seconds;
        last_bytes_ = bytes;
    }

    // Bytes per second, 0 while unknown.
    double bytes_per_second() const {
        return has_rate_ && last_seconds_ - first_seconds_ >= kWarmupSeconds ? rate_ : 0.0;
    }

    // Seconds until `remaining` more bytes arrive; negative when there is no rate yet.
    double seconds_left(uint64_t remaining) const {
        const double rate = bytes_per_second();
        if (rate <= 0.0) return -1.0;
        return static_cast<double>(remaining) / rate;
    }

private:
    bool started_ = false;
    bool has_rate_ = false;
    double first_seconds_ = 0.0;
    double last_seconds_ = 0.0;
    uint64_t last_bytes_ = 0;
    double rate_ = 0.0;
};

// "about 5 min left", "less than a minute left"; empty for an unknown (negative)
// estimate.
inline std::string eta_text(double seconds) {
    if (seconds < 0.0) return std::string();
    if (seconds < 60.0) return "less than a minute left";
    const long minutes = std::lround(seconds / 60.0);
    return "about " + std::to_string(minutes) + " min left";
}

}  // namespace splat
