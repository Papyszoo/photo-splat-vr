// The "smallest three" quaternion packing, shared by .spz v3+ and
// .compressed.ply.
//
// Both formats store three components and rebuild the largest from the unit
// norm, but they do not share the tuple order or the field encoding (SPEC
// Appendix A.2 and A.3), so there are two named entry points here rather than
// one function with a silent convention. Four copies of the same loop with an
// unstated order is how the spz reader ended up with the compressed-PLY
// encoding.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace splat {

// Niantic spz v3/v4, after `unpackQuaternionSmallestThree` in
// third_party reference src/cc/splat-utils.h. `q` is x, y, z, w: the 2-bit
// index selects the largest of that tuple, and each of the other three is a
// 9-bit magnitude plus a sign bit, value = +/- (mag / 511) / sqrt(2).
void unpack_smallest_three_spz(uint32_t packed, float* q_xyzw);

// SuperSplat / PlayCanvas .compressed.ply. `q` is w, x, y, z: the 2-bit index
// selects the largest of that tuple and each of the other three is a 10-bit
// value centred on 511.5, (n / 1023 - 0.5) * sqrt(2).
void unpack_smallest_three_compressed_ply(uint32_t packed, float* q_wxyz);

}  // namespace splat
