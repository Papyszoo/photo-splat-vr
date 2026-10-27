#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <vector>

#include "common/half.h"

// `float_to_half` / `half_to_float` define the bytes of a .psplat and of the
// renderer's fp16 storage buffers, so their rounding has to be exact and
// identical on every toolchain. The exhaustive checks below use the other
// direction as the oracle: every adjacent pair of half values pins the
// round-to-nearest-even decision at its midpoint.

namespace {

float next_float(float value, bool up) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    // Both operands are positive here, so the bit pattern orders like the value.
    bits += up ? 1u : 0u;
    if (!up && bits > 0) bits -= 1u;
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

}  // namespace

TEST_CASE("fp16 round trip covers every finite half value") {
    for (uint32_t bits = 0; bits <= 0x7bffu; ++bits) {  // positive finite halves
        const float value = splat::half_to_float(static_cast<uint16_t>(bits));
        CHECK(splat::float_to_half(value) == static_cast<uint16_t>(bits));
        const float negative = -value;
        CHECK(splat::float_to_half(negative) == static_cast<uint16_t>(bits | 0x8000u));
    }
}

TEST_CASE("fp16 rounds to nearest, ties to even") {
    // Half values are exact in float32, and so is the midpoint between two
    // adjacent ones, which makes the boundary check exact.
    for (uint32_t bits = 0; bits < 0x7bffu; ++bits) {
        const float low = splat::half_to_float(static_cast<uint16_t>(bits));
        const float high = splat::half_to_float(static_cast<uint16_t>(bits + 1));
        const float mid = 0.5f * (low + high);
        // Sign of the tie: even keeps the even mantissa, i.e. (bits & 1) == 0.
        const uint16_t tied = static_cast<uint16_t>((bits & 1u) == 0 ? bits : bits + 1);
        CHECK_MESSAGE(splat::float_to_half(mid) == tied,
                      "tie at half 0x" << std::hex << bits << std::dec);
        if (bits != 0) {  // no float strictly between 0 and the smallest subnormal
            CHECK(splat::float_to_half(next_float(mid, false)) == static_cast<uint16_t>(bits));
        }
        CHECK(splat::float_to_half(next_float(mid, true)) == static_cast<uint16_t>(bits + 1));
    }
}

TEST_CASE("fp16 special values") {
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(splat::float_to_half(inf) == 0x7c00u);
    CHECK(splat::float_to_half(-inf) == 0xfc00u);
    CHECK(splat::half_to_float(0x7c00u) == inf);
    CHECK(splat::half_to_float(0xfc00u) == -inf);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const uint16_t nan_half = splat::float_to_half(nan);
    CHECK((nan_half & 0x7c00u) == 0x7c00u);
    CHECK((nan_half & 0x03ffu) != 0u);          // still a NaN, not an infinity
    CHECK(std::isnan(splat::half_to_float(nan_half)));
    const uint16_t negative_nan = splat::float_to_half(-nan);
    CHECK((negative_nan & 0x8000u) == 0x8000u);
    CHECK((negative_nan & 0x03ffu) != 0u);

    // Signed zero survives in both directions.
    CHECK(splat::float_to_half(0.0f) == 0x0000u);
    CHECK(splat::float_to_half(-0.0f) == 0x8000u);
    CHECK(std::signbit(splat::half_to_float(0x8000u)));
}

TEST_CASE("fp16 overflow and underflow edges") {
    // Largest finite half, and the first value that must round to infinity.
    CHECK(splat::half_to_float(0x7bffu) == 65504.0f);
    CHECK(splat::float_to_half(65504.0f) == 0x7bffu);
    CHECK(splat::float_to_half(65519.0f) == 0x7bffu);   // below the tie
    CHECK(splat::float_to_half(65520.0f) == 0x7c00u);   // tie at 65504 + 16 ulp -> even is inf
    CHECK(splat::float_to_half(65536.0f) == 0x7c00u);
    CHECK(splat::float_to_half(1.0e30f) == 0x7c00u);

    // Smallest subnormal, the first normal, and the underflow to zero.
    CHECK(splat::half_to_float(0x0001u) == std::ldexp(1.0f, -24));
    CHECK(splat::float_to_half(std::ldexp(1.0f, -24)) == 0x0001u);
    CHECK(splat::float_to_half(std::ldexp(1.0f, -25)) == 0x0000u);  // tie -> even (zero)
    CHECK(splat::float_to_half(std::ldexp(1.0f, -26)) == 0x0000u);
    CHECK(splat::half_to_float(0x0400u) == std::ldexp(1.0f, -14));
    CHECK(splat::float_to_half(std::ldexp(1.0f, -14)) == 0x0400u);
    // A float subnormal is far below the half range.
    CHECK(splat::float_to_half(std::numeric_limits<float>::denorm_min()) == 0x0000u);
}

TEST_CASE("log-scale and opacity logits keep their meaning through fp16") {
    // The two attributes the renderer feeds through exp()/sigmoid(): a logit of
    // +/-40 must stay finite and saturate the right way, and a log-scale of 15
    // (e^15 m) must survive as itself, not as an infinity.
    for (float logit : {-80.0f, -40.0f, -12.0f, 0.0f, 12.0f, 40.0f, 80.0f}) {
        const float stored = splat::half_to_float(splat::float_to_half(logit));
        CHECK(std::isfinite(stored));
        CHECK(stored == doctest::Approx(logit).epsilon(1e-3));
    }
    for (float log_scale : {-30.0f, -12.0f, -5.0f, 0.0f, 5.0f, 11.0f, 15.0f}) {
        const float stored = splat::half_to_float(splat::float_to_half(log_scale));
        CHECK(std::isfinite(stored));
        CHECK(std::exp(stored) == doctest::Approx(std::exp(log_scale)).epsilon(1e-2));
    }
    // Beyond the fp16 range the value saturates to infinity; exp(inf) = inf and
    // sigmoid(+inf) = 1, so the renderer still culls or keeps the splat instead
    // of producing a NaN from garbage input.
    CHECK(splat::float_to_half(1.0e6f) == 0x7c00u);
    CHECK(splat::float_to_half(-1.0e6f) == 0xfc00u);
}
