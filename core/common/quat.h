// Small unit-quaternion helpers in the order the splat files and the shaders
// use: (w, x, y, z). The renderer's covariance is R S S^T R^T
// (core/render/project_math.h), so a world rotation composes on the left:
// R(qz * q) = R(qz) R(q). Keeping the Hamilton product in one documented place
// is what makes that order checkable; the component order of the display
// transform's helpers (core/render/grab.cpp, (x, y, z, w)) is a different one and
// stays there.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>

namespace splat {

// Hamilton product, out = a * b in (w, x, y, z) order. Rotations compose as
// R(a*b) = R(a) R(b): `quat_mul(out, qz, q)` turns q in the world frame.
inline void quat_mul(float out[4], const float a[4], const float b[4]) {
    const float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    const float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    const float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    const float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    out[0] = w;
    out[1] = x;
    out[2] = y;
    out[3] = z;
}

// Normalises q into out; false when q is not a usable rotation, in which case
// out is the identity. A zero quaternion arrives from a damaged file.
inline bool quat_unit(float out[4], const float q[4]) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(length) || !(length > 1.0e-6f)) {
        out[0] = 1.0f;
        out[1] = out[2] = out[3] = 0.0f;
        return false;
    }
    for (int i = 0; i < 4; ++i) out[i] = q[i] / length;
    return true;
}

}  // namespace splat
