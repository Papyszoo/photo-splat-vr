// The model code's shared text helpers (core/model/text_format.h): one copy of
// the suffix test and the byte / rate / duration numbers the file converter, the
// streaming converter and the model manager print in their "model fetch:" and
// "stream:" klog lines. The copies this replaced had two rate precisions
// ("%.1f" and "%.2f MB/s"); the shape pinned here is the one every path now
// prints.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "model/text_format.h"

TEST_CASE("model text: ends_with is a true suffix test") {
    CHECK(splat::ends_with("encoder.image_encoder.head.weight",
                           "encoder.image_encoder.head.weight"));
    CHECK(splat::ends_with("data/7", "/7"));
    CHECK(splat::ends_with("data.pkl", "data.pkl"));
    CHECK_FALSE(splat::ends_with("data.pkl", "/data.pkl"));
    CHECK_FALSE(splat::ends_with("short", "a longer suffix"));
    CHECK_FALSE(splat::ends_with("", "x"));
    CHECK(splat::ends_with("anything", ""));  // an empty suffix matches
}

TEST_CASE("model text: sizes, rates and seconds keep one klog shape") {
    // GB is bytes / 1e9 with two decimals (the converter's and the manager's
    // "in"/"written" numbers).
    CHECK(splat::gigabyte_text(1'312'705'920ull) == "1.31 GB");
    CHECK(splat::gigabyte_text(0) == "0.00 GB");
    // MB is bytes / 1e6 with one decimal (transfer sizes).
    CHECK(splat::megabyte_text(2'809'738'232ull) == "2809.7 MB");
    CHECK(splat::megabyte_text(0) == "0.0 MB");
    // The rate has one precision everywhere now; both converters printed this
    // one and the manager printed two decimals.
    CHECK(splat::rate_text(100'000'000ull, 2.0) == "50.0 MB/s");
    CHECK(splat::rate_text(0, 0.0) == "? MB/s");
    CHECK(splat::rate_text(5, -1.0) == "? MB/s");
    CHECK(splat::seconds_text(13.793) == "13.8");
    CHECK(splat::seconds_text(0.0) == "0.0");
}
