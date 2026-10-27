#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "render/project_math.h"

// The EWA projection in shaders/project.comp is mirrored by
// core/render/project_math.h. These cases pin the two mistakes that a
// column-major GLSL constructor makes easy: a transposed rotation matrix
// (R^T S^2 R instead of R S^2 R^T) and a transposed Jacobian (which drops the
// -fx*x/z^2 and -fy*y/z^2 depth-gradient terms).

namespace {

// Independent of project_math.h: project a camera-space point with the pinhole
// model and difference it numerically. The result must equal the analytic
// Jacobian the header uses.
void numeric_jacobian(const float p[3], float fx, float fy, float jacobian[2][3]) {
    const float h = 1e-4f;
    for (int axis = 0; axis < 3; ++axis) {
        float up[3] = {p[0], p[1], p[2]};
        float down[3] = {p[0], p[1], p[2]};
        up[axis] += h;
        down[axis] -= h;
        jacobian[0][axis] = (fx * up[0] / up[2] - fx * down[0] / down[2]) / (2.0f * h);
        jacobian[1][axis] = (fy * up[1] / up[2] - fy * down[1] / down[2]) / (2.0f * h);
    }
}

// Naive J cov J^T with the numeric Jacobian, so a shared transcription bug in
// the header cannot hide.
splat::Covariance2D numeric_projection(const splat::Mat3& cov, const float p[3], float fx,
                                       float fy) {
    float jacobian[2][3];
    numeric_jacobian(p, fx, fy, jacobian);
    splat::Covariance2D out;
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 2; ++c) {
            float sum = 0.0f;
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    sum += jacobian[r][i] * cov.at(i, k) * jacobian[c][k];
                }
            }
            if (r == 0 && c == 0) out.a = sum;
            if (r == 1 && c == 1) out.c = sum;
            if (r == 0 && c == 1) out.b = sum;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("quaternion_to_rotation builds R, not R^T") {
    // 90 degrees about x: w = c, x = s with cos(45) = sin(45) = sqrt(1/2).
    const float c = std::sqrt(0.5f);
    const splat::Mat3 r = splat::quaternion_to_rotation(c, c, 0.0f, 0.0f);
    // R maps +y to +z and +z to -y.
    CHECK(r.at(0, 0) == doctest::Approx(1.0f));
    CHECK(std::abs(r.at(1, 1)) < 1e-6f);
    CHECK(r.at(1, 2) == doctest::Approx(-1.0f));
    CHECK(r.at(2, 1) == doctest::Approx(1.0f));
    CHECK(std::abs(r.at(2, 2)) < 1e-6f);
    // A rotation is orthogonal with det +1, while the transpose has det -1 in
    // this particular case.
    const float det = r.at(0, 0) * (r.at(1, 1) * r.at(2, 2) - r.at(1, 2) * r.at(2, 1)) -
                      r.at(0, 1) * (r.at(1, 0) * r.at(2, 2) - r.at(1, 2) * r.at(2, 0)) +
                      r.at(0, 2) * (r.at(1, 0) * r.at(2, 1) - r.at(1, 1) * r.at(2, 0));
    CHECK(det == doctest::Approx(1.0f));
}

TEST_CASE("depth-only covariance keeps the foreshortening terms") {
    // The hand derivation from the review: P = (1, 2, 2), f = 100, a Gaussian
    // elongated along z. J has to contribute -fx*x/z^2 and -fy*y/z^2, which a
    // transposed Jacobian puts outside the 2x2 block.
    splat::Mat3 cov;  // identity, then only the zz term
    cov.at(0, 0) = 0.0f;
    cov.at(1, 1) = 0.0f;
    cov.at(2, 2) = 1.0f;

    const splat::Covariance2D projected =
        splat::project_covariance_2d(cov, 1.0f, 2.0f, 2.0f, 100.0f, 100.0f, 0.0f);
    CHECK(projected.a == doctest::Approx(625.0f));
    CHECK(projected.b == doctest::Approx(1250.0f));
    CHECK(projected.c == doctest::Approx(2500.0f));
}

TEST_CASE("off-axis rotated anisotropic Gaussian matches a numeric Jacobian") {
    // A 30 degree turn about z composed with 40 degrees about x.
    const float angle_z = 30.0f * 3.14159265358979323846f / 180.0f;
    const float angle_x = 40.0f * 3.14159265358979323846f / 180.0f;
    const float cz = std::cos(0.5f * angle_z);
    const float sz = std::sin(0.5f * angle_z);
    const float cx = std::cos(0.5f * angle_x);
    const float sx = std::sin(0.5f * angle_x);
    // q = q_z * q_x
    const float w = cz * cx;
    const float x = cz * sx;
    const float y = sz * sx;
    const float z = -sz * cx;

    const float scales[3] = {0.09f, 0.045f, 0.02f};
    const splat::Mat3 rotation = splat::quaternion_to_rotation(w, x, y, z);
    const splat::Mat3 cov =
        splat::gaussian_covariance_3d(rotation, scales[0], scales[1], scales[2]);

    // Off axis in both x and y, so every term of J is exercised.
    const float position[3] = {0.61f, -0.37f, 2.05f};
    const float fx = 1074.29f;
    const float fy = 1041.13f;

    const splat::Covariance2D projected = splat::project_covariance_2d(
        cov, position[0], position[1], position[2], fx, fy, 0.0f);
    const splat::Covariance2D expected = numeric_projection(cov, position, fx, fy);

    // The numeric Jacobian is a central difference with h = 1e-4, so it agrees
    // with the analytic one to about 2e-4 relative. A transposed R or J is off
    // by tens of percent, far outside this window.
    CHECK(projected.a == doctest::Approx(expected.a).epsilon(1e-3));
    CHECK(projected.b == doctest::Approx(expected.b).epsilon(1e-3));
    CHECK(projected.c == doctest::Approx(expected.c).epsilon(1e-3));

    // The splat is far larger in x than in y before projection: 0.09 m at
    // 2.05 m with f = 1074 px is about 47 px. The projected covariance must
    // carry that, and the off-diagonal term must be non-zero off axis.
    CHECK(projected.a > 1500.0f);
    CHECK(projected.c > 400.0f);
    CHECK(std::abs(projected.b) > 20.0f);

    // Transposing the rotation would give a different orientation but the same
    // eigenvalues; rotating x into y does change the projected footprint, so
    // the check above is what actually pins R versus R^T.
    const splat::Mat3 transposed = splat::transpose(rotation);
    const splat::Mat3 wrong_cov =
        splat::gaussian_covariance_3d(transposed, scales[0], scales[1], scales[2]);
    const splat::Covariance2D wrong = splat::project_covariance_2d(
        wrong_cov, position[0], position[1], position[2], fx, fy, 0.0f);
    CHECK(std::abs(wrong.a - projected.a) > 1.0f);
}

TEST_CASE("covariance axes describe a rotated ellipse") {
    // Diagonal covariance: axes are the coordinate axes.
    splat::Covariance2D diagonal;
    diagonal.a = 9.0f;
    diagonal.c = 4.0f;
    splat::CovarianceAxes axes = splat::covariance_axes(diagonal);
    CHECK(axes.axis0[0] == doctest::Approx(3.0f));
    CHECK(std::abs(axes.axis0[1]) < 1e-6f);
    CHECK(std::abs(axes.axis1[0]) < 1e-6f);
    CHECK(std::abs(axes.axis1[1]) == doctest::Approx(2.0f));

    // Rotate that ellipse by 35 degrees: R diag(9, 4) R^T.
    const float theta = 35.0f * 3.14159265358979323846f / 180.0f;
    const float ct = std::cos(theta);
    const float st = std::sin(theta);
    splat::Covariance2D rotated;
    rotated.a = 9.0f * ct * ct + 4.0f * st * st;
    rotated.b = (9.0f - 4.0f) * ct * st;
    rotated.c = 9.0f * st * st + 4.0f * ct * ct;

    axes = splat::covariance_axes(rotated);
    CHECK(axes.lambda0 == doctest::Approx(9.0f).epsilon(1e-5));
    CHECK(axes.lambda1 == doctest::Approx(4.0f).epsilon(1e-5));
    CHECK(axes.axis0[0] == doctest::Approx(3.0f * ct));
    CHECK(axes.axis0[1] == doctest::Approx(3.0f * st));

    // The axes are the eigenvectors of the covariance.
    for (int i = 0; i < 2; ++i) {
        const float* axis = i == 0 ? axes.axis0 : axes.axis1;
        const float lambda = i == 0 ? axes.lambda0 : axes.lambda1;
        const float x = rotated.a * axis[0] + rotated.b * axis[1];
        const float y = rotated.b * axis[0] + rotated.c * axis[1];
        CHECK(x == doctest::Approx(lambda * axis[0]).epsilon(1e-5));
        CHECK(y == doctest::Approx(lambda * axis[1]).epsilon(1e-5));
    }
}

TEST_CASE("the low-pass term is added to the diagonal only") {
    splat::Mat3 cov;
    cov.at(0, 1) = 2.0f;
    cov.at(1, 0) = 2.0f;
    const splat::Covariance2D plain =
        splat::project_covariance_2d(cov, 0.0f, 0.0f, 1.0f, 512.0f, 512.0f, 0.0f);
    const splat::Covariance2D filtered =
        splat::project_covariance_2d(cov, 0.0f, 0.0f, 1.0f, 512.0f, 512.0f, 0.3f);
    CHECK(filtered.a == doctest::Approx(plain.a + 0.3f));
    CHECK(filtered.c == doctest::Approx(plain.c + 0.3f));
    CHECK(filtered.b == doctest::Approx(plain.b));
}
