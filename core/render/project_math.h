#pragma once

// CPU reference for the 3DGS / EWA projection that `shaders/project.comp`
// implements. The shader and this header must agree: the unit tests compare
// them, and the shader comments point here. Header-only so the tests do not
// need a Vulkan device to check the maths.

#include <cmath>

namespace splat {

// 3x3 matrix, row-major: m[row * 3 + col]. GLSL's mat3 constructor takes
// columns, which is exactly the mistake this type exists to avoid.
struct Mat3 {
    float m[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    float at(int row, int col) const { return m[row * 3 + col]; }
    float& at(int row, int col) { return m[row * 3 + col]; }
};

inline Mat3 operator*(const Mat3& a, const Mat3& b) {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += a.at(r, k) * b.at(k, c);
            out.at(r, c) = sum;
        }
    }
    return out;
}

inline Mat3 transpose(const Mat3& a) {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out.at(r, c) = a.at(c, r);
    }
    return out;
}

// Rotation matrix of the unit quaternion (w, x, y, z), matching the SHARP
// convention: cov3d = R S S^T R^T with S = diag(sigma).
inline Mat3 quaternion_to_rotation(float w, float x, float y, float z) {
    Mat3 r;
    r.at(0, 0) = 1.0f - 2.0f * (y * y + z * z);
    r.at(0, 1) = 2.0f * (x * y - w * z);
    r.at(0, 2) = 2.0f * (x * z + w * y);
    r.at(1, 0) = 2.0f * (x * y + w * z);
    r.at(1, 1) = 1.0f - 2.0f * (x * x + z * z);
    r.at(1, 2) = 2.0f * (y * z - w * x);
    r.at(2, 0) = 2.0f * (x * z - w * y);
    r.at(2, 1) = 2.0f * (y * z + w * x);
    r.at(2, 2) = 1.0f - 2.0f * (x * x + y * y);
    return r;
}

// Symmetric 2D covariance in pixels^2: [[a, b], [b, c]].
struct Covariance2D {
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
};

// EWA projection of a camera-space covariance to pixels:
//
//   J = [[fx/z, 0, -fx*x/z^2],
//        [0, fy/z, -fy*y/z^2]]
//   Sigma2D = J cov3d J^T, then `eps` is added to the diagonal (gsplat's
//   eps2d low-pass term; SHARP's renderer passes 0.0).
inline Covariance2D project_covariance_2d(const Mat3& cov3d, float x, float y, float z,
                                          float fx, float fy, float eps) {
    const float inv_z = 1.0f / z;
    Mat3 jacobian;
    jacobian.at(0, 0) = fx * inv_z;
    jacobian.at(0, 1) = 0.0f;
    jacobian.at(0, 2) = -fx * x * inv_z * inv_z;
    jacobian.at(1, 0) = 0.0f;
    jacobian.at(1, 1) = fy * inv_z;
    jacobian.at(1, 2) = -fy * y * inv_z * inv_z;
    jacobian.at(2, 0) = 0.0f;
    jacobian.at(2, 1) = 0.0f;
    jacobian.at(2, 2) = 0.0f;

    const Mat3 projected = jacobian * cov3d * transpose(jacobian);
    Covariance2D out;
    out.a = projected.at(0, 0) + eps;
    out.b = projected.at(0, 1);
    out.c = projected.at(1, 1) + eps;
    return out;
}

// The two semi-axis vectors of the projected splat, in pixels, oriented like
// the shader's `axis0` / `axis1`. The 3-sigma quad spans +/-3*axis.
struct CovarianceAxes {
    float axis0[2] = {0.0f, 0.0f};
    float axis1[2] = {0.0f, 0.0f};
    float lambda0 = 0.0f;  // eigenvalue of axis0 (larger)
    float lambda1 = 0.0f;  // eigenvalue of axis1
};

inline CovarianceAxes covariance_axes(const Covariance2D& cov) {
    const float det = cov.a * cov.c - cov.b * cov.b;
    const float mid = 0.5f * (cov.a + cov.c);
    const float disc = std::sqrt(std::fmax(0.0f, mid * mid - det));
    const float lambda0 = mid + disc;
    const float lambda1 = mid - disc;
    const float s0 = std::sqrt(std::fmax(lambda0, 0.0f));
    const float s1 = std::sqrt(std::fmax(lambda1, 0.0f));
    const float theta = 0.5f * std::atan2(2.0f * cov.b, cov.a - cov.c);
    const float cos_theta = std::cos(theta);
    const float sin_theta = std::sin(theta);

    CovarianceAxes axes;
    axes.axis0[0] = s0 * cos_theta;
    axes.axis0[1] = s0 * sin_theta;
    axes.axis1[0] = -s1 * sin_theta;
    axes.axis1[1] = s1 * cos_theta;
    axes.lambda0 = lambda0;
    axes.lambda1 = lambda1;
    return axes;
}

// 3D covariance R S S^T R^T from a unit quaternion and per-axis scale.
inline Mat3 gaussian_covariance_3d(const Mat3& rotation, float sx, float sy, float sz) {
    Mat3 scale;
    scale.at(0, 0) = sx * sx;
    scale.at(1, 1) = sy * sy;
    scale.at(2, 2) = sz * sz;
    return rotation * scale * transpose(rotation);
}

}  // namespace splat
