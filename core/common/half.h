#pragma once

#include <cstdint>
#include <cstring>

namespace splat {

// IEEE 754 binary16 conversions, round-to-nearest-even. Written out instead of
// using _Float16 so the .psplat bytes are identical on every toolchain.
inline uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu);
    const uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xff) {  // inf or NaN
        if (mantissa == 0) return static_cast<uint16_t>(sign | 0x7c00u);
        uint32_t payload = mantissa >> 13;
        if (payload == 0) payload = 1;  // keep NaN a NaN
        return static_cast<uint16_t>(sign | 0x7c00u | payload);
    }

    int32_t half_exponent = exponent - 127 + 15;
    if (half_exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);

    if (half_exponent <= 0) {
        if (half_exponent < -10) return static_cast<uint16_t>(sign);
        const uint32_t full = mantissa | 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - half_exponent);
        uint32_t half = full >> shift;
        const uint32_t remainder = full & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (half & 1u))) ++half;
        return static_cast<uint16_t>(sign | half);
    }

    uint32_t half = sign | (static_cast<uint32_t>(half_exponent) << 10) | (mantissa >> 13);
    const uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(half & 0xffffu);
}

inline float half_to_float(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x3ffu;
    uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

}  // namespace splat
