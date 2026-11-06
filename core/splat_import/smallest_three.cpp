// SPDX-License-Identifier: GPL-3.0-or-later

#include "splat_import/smallest_three.h"

#include <algorithm>
#include <cmath>

namespace splat {
namespace {

constexpr float kSqrt1_2 = 0.70710678118654752440f;
constexpr float kSqrt2 = 1.41421356237309504880f;

// The component that was not stored follows from the unit norm. A corrupt
// packing whose three fields already exceed 1 comes back as 0 instead of NaN,
// so a bad file still yields a usable quaternion.
void fill_largest(float* q, uint32_t largest) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != largest) sum += q[i] * q[i];
    }
    q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
}

}  // namespace

void unpack_smallest_three_spz(uint32_t packed, float* q_xyzw) {
    const uint32_t largest = (packed >> 30) & 0x3u;
    // The packer walks the tuple from index 3 down, taking the lowest 10 bits
    // first (spz v3's in-place `comp` shift), so index 3 receives the low field.
    for (uint32_t i = 4; i-- > 0;) {
        if (i == largest) continue;
        const uint32_t magnitude = packed & 0x1FFu;  // 9 bits
        const bool negative = ((packed >> 9) & 0x1u) != 0;
        packed >>= 10;
        float value = static_cast<float>(magnitude) / 511.0f * kSqrt1_2;
        if (negative) value = -value;
        q_xyzw[i] = value;
    }
    fill_largest(q_xyzw, largest);
}

void unpack_smallest_three_compressed_ply(uint32_t packed, float* q_wxyz) {
    const uint32_t largest = (packed >> 30) & 0x3u;
    const uint32_t fields[3] = {(packed >> 20) & 0x3FFu, (packed >> 10) & 0x3FFu,
                                packed & 0x3FFu};
    size_t field = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (i == largest) continue;
        q_wxyz[i] = (static_cast<float>(fields[field++]) / 1023.0f - 0.5f) * kSqrt2;
    }
    fill_largest(q_wxyz, largest);
}

}  // namespace splat
