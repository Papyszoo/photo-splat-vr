// The Models row's download time estimate: the smoothed byte rate and the text.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>

#include "ui/download_eta.h"

using splat::DownloadRate;
using splat::eta_text;

namespace {

// Feeds `seconds` of a steady rate, one sample every half second; returns the
// byte count at the end.
uint64_t feed(DownloadRate& rate, double from, double seconds, double bytes_per_second, uint64_t bytes) {
    for (double t = 0.5; t <= seconds + 1e-9; t += 0.5) {
        bytes += static_cast<uint64_t>(bytes_per_second * 0.5);
        rate.add(from + t, bytes);
    }
    return bytes;
}

}  // namespace

TEST_CASE("download eta: a steady rate gives the time left") {
    DownloadRate rate;
    CHECK(rate.seconds_left(1000) < 0.0);  // nothing seen yet
    const uint64_t bytes = feed(rate, 0.0, 20.0, 5e6, 0);
    CHECK(rate.bytes_per_second() == doctest::Approx(5e6).epsilon(0.01));
    // 2.8 GB total: 100 MB done is about 9 minutes at 5 MB/s.
    const uint64_t remaining = 2800000000ull - bytes;
    CHECK(rate.seconds_left(remaining) == doctest::Approx(static_cast<double>(remaining) / 5e6).epsilon(0.01));
    CHECK(eta_text(rate.seconds_left(remaining)) == "about 9 min left");
}

TEST_CASE("download eta: no estimate until a few seconds were seen") {
    DownloadRate rate;
    feed(rate, 0.0, 2.5, 5e6, 0);
    CHECK(rate.seconds_left(1000000) < 0.0);
    CHECK(eta_text(rate.seconds_left(1000000)).empty());
    feed(rate, 2.5, 2.0, 5e6, 12500000);
    CHECK(rate.seconds_left(1000000) > 0.0);
}

TEST_CASE("download eta: a resumed download does not count the bytes it already had") {
    DownloadRate rate;
    uint64_t bytes = 1000000000ull;  // the first sample is only the starting point
    rate.add(0.0, bytes);
    bytes = feed(rate, 0.0, 10.0, 4e6, bytes);
    CHECK(rate.bytes_per_second() == doctest::Approx(4e6).epsilon(0.01));
}

TEST_CASE("download eta: the rate follows a change in speed without jumping") {
    DownloadRate rate;
    uint64_t bytes = feed(rate, 0.0, 20.0, 5e6, 0);
    bytes = feed(rate, 20.0, 2.0, 10e6, bytes);
    const double soon = rate.bytes_per_second();
    CHECK(soon > 5e6);
    CHECK(soon < 8e6);  // a couple of seconds do not double the estimate
    feed(rate, 22.0, 40.0, 10e6, bytes);
    CHECK(rate.bytes_per_second() == doctest::Approx(10e6).epsilon(0.05));
}

TEST_CASE("download eta: a stalled transfer has no estimate") {
    DownloadRate rate;
    uint64_t bytes = feed(rate, 0.0, 6.0, 5e6, 0);
    // No new bytes for a long time: the average falls toward zero, and a rate of
    // exactly zero is "unknown", not an infinite wait.
    for (double t = 7.0; t <= 400.0; t += 1.0) rate.add(t, bytes);
    CHECK(rate.bytes_per_second() < 1e3);
}

TEST_CASE("download eta: the text") {
    CHECK(eta_text(-1.0).empty());
    CHECK(eta_text(0.0) == "less than a minute left");
    CHECK(eta_text(59.0) == "less than a minute left");
    CHECK(eta_text(60.0) == "about 1 min left");
    CHECK(eta_text(260.0) == "about 4 min left");
    CHECK(eta_text(300.0) == "about 5 min left");
    CHECK(eta_text(3 * 3600.0) == "about 180 min left");
}
