#include "recon/sharp/sharp_composer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace splat {
namespace {

// compose parameters from PredictorParams / DeltaFactor.
constexpr double kDeltaXY = 1e-3;
constexpr double kDeltaZ = 1e-3;
constexpr double kDeltaColor = 0.1;
constexpr double kDeltaOpacity = 1.0;
constexpr double kDeltaScale = 1.0;
constexpr double kDeltaQuaternion = 1.0;
constexpr double kMinScale = 0.0;
constexpr double kMaxScale = 10.0;
constexpr double kColorMin = 0.01;
constexpr double kColorMax = 0.99;
constexpr double kShDcScale = 0.28209479177387814;  // sqrt(1 / (4 pi))

// _get_scale_activation_constant(max_scale, min_scale).
double scale_activation_a() { return (kMaxScale - kMinScale) / (1.0 - kMinScale) / (kMaxScale - 1.0); }
double scale_activation_b() {
    const double target = (1.0 - kMinScale) / (kMaxScale - kMinScale);
    return std::log(target / (1.0 - target));
}

double sigmoid(double value) { return 1.0 / (1.0 + std::exp(-value)); }

double inverse_sigmoid(double value) { return std::log(value / (1.0 - value)); }

// utils/math.py:62-67, evaluated exactly rather than through the reference's
// sigmoid round trip: inverse_softplus(t) = t + log(1 - exp(-t)) with the input
// clamped at 1e-6. The reference computes 1 - exp(-t) from a sigmoid, which is
// the same expression; log1p(-exp(-t)) is the numerically careful form.
double inverse_softplus(double value) {
    const double clamped = std::max(value, 1e-6);
    return clamped + std::log1p(-std::exp(-clamped));
}

double softplus(double value) {
    // F.softplus(x) = log(1 + exp(x)), with PyTorch's threshold at 20.
    if (value > 20.0) return value;
    return std::log1p(std::exp(value));
}

// A 3x3 symmetric matrix eigen-decomposition by cyclic Jacobi rotations, fp64.
// Returns eigenvalues in descending order and the eigenvectors as the columns
// of `vectors`; `matrix` is preserved except for `vectors` filling.
void symmetric_eigen(double matrix[3][3], double eigenvalues[3], double vectors[3][3]) {
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            vectors[row][column] = row == column ? 1.0 : 0.0;
        }
    }
    for (int sweep = 0; sweep < 32; ++sweep) {
        const double off = std::fabs(matrix[0][1]) + std::fabs(matrix[0][2]) + std::fabs(matrix[1][2]);
        if (off < 1e-300) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(matrix[p][q]) <= 1e-300) continue;
                const double theta = (matrix[q][q] - matrix[p][p]) / (2.0 * matrix[p][q]);
                const double sign = theta >= 0.0 ? 1.0 : -1.0;
                const double t = sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double a = matrix[k][p];
                    const double b = matrix[k][q];
                    matrix[k][p] = c * a - s * b;
                    matrix[k][q] = s * a + c * b;
                }
                for (int k = 0; k < 3; ++k) {
                    const double a = matrix[p][k];
                    const double b = matrix[q][k];
                    matrix[p][k] = c * a - s * b;
                    matrix[q][k] = s * a + c * b;
                }
                for (int k = 0; k < 3; ++k) {
                    const double a = vectors[k][p];
                    const double b = vectors[k][q];
                    vectors[k][p] = c * a - s * b;
                    vectors[k][q] = s * a + c * b;
                }
            }
        }
    }
    int order[3] = {0, 1, 2};
    const double diagonal[3] = {matrix[0][0], matrix[1][1], matrix[2][2]};
    std::sort(order, order + 3, [&](int a, int b) { return diagonal[a] > diagonal[b]; });

    double sorted_vectors[3][3];
    for (int column = 0; column < 3; ++column) {
        eigenvalues[column] = diagonal[order[column]];
        for (int row = 0; row < 3; ++row) sorted_vectors[row][column] = vectors[row][order[column]];
    }
    // A right-handed basis, like the reference's reflection fix.
    const double determinant =
        sorted_vectors[0][0] * (sorted_vectors[1][1] * sorted_vectors[2][2] -
                                sorted_vectors[1][2] * sorted_vectors[2][1]) -
        sorted_vectors[0][1] * (sorted_vectors[1][0] * sorted_vectors[2][2] -
                                sorted_vectors[1][2] * sorted_vectors[2][0]) +
        sorted_vectors[0][2] * (sorted_vectors[1][0] * sorted_vectors[2][1] -
                                sorted_vectors[1][1] * sorted_vectors[2][0]);
    if (determinant < 0.0) {
        for (int row = 0; row < 3; ++row) sorted_vectors[row][2] = -sorted_vectors[row][2];
    }
    std::memcpy(vectors, sorted_vectors, sizeof(sorted_vectors));
}

// Rotation matrix (row-major) of a quaternion w, x, y, z, normalised first.
void rotation_from_quaternion(const double quaternion[4], double matrix[3][3]) {
    double norm = std::sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                            quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
    if (!(norm > 0.0)) norm = 1.0;
    const double w = quaternion[0] / norm;
    const double x = quaternion[1] / norm;
    const double y = quaternion[2] / norm;
    const double z = quaternion[3] / norm;
    const double values[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y - w * z),         2 * (x * z + w * y),
        2 * (x * y + w * z),     1 - 2 * (x * x + z * z),     2 * (y * z - w * x),
        2 * (x * z - w * y),     2 * (y * z + w * x),         1 - 2 * (x * x + y * y)};
    std::memcpy(matrix, values, sizeof(values));
}

// Quaternion (w, x, y, z) of a rotation matrix, w >= 0. Same as
// utils/linalg.py:41-59 with the sign fixed for determinism.
void quaternion_from_rotation(const double matrix[3][3], float out[4]) {
    const double m00 = matrix[0][0], m01 = matrix[0][1], m02 = matrix[0][2];
    const double m10 = matrix[1][0], m11 = matrix[1][1], m12 = matrix[1][2];
    const double m20 = matrix[2][0], m21 = matrix[2][1], m22 = matrix[2][2];
    const double trace = m00 + m11 + m22;
    double w = 0.0, x = 0.0, y = 0.0, z = 0.0;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s;
        x = 0.25 * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25 * s;
        z = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25 * s;
    }
    if (w < 0.0) {
        w = -w;
        x = -x;
        y = -y;
        z = -z;
    }
    out[0] = static_cast<float>(w);
    out[1] = static_cast<float>(x);
    out[2] = static_cast<float>(y);
    out[3] = static_cast<float>(z);
}

}  // namespace

float srgb_to_linear(float value) {
    // sharp/utils/color_space.py:31-58.
    constexpr float kThreshold = 0.04045f;
    if (value <= kThreshold) return value / 12.92f;
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    // sharp/utils/color_space.py:61-88.
    constexpr float kThreshold = 0.0031308f;
    if (value <= kThreshold) return value * 12.92f;
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

Result<SharpComposedGaussians> sharp_compose(const SharpBaseValues& base, const float* delta) {
    using ResultT = Result<SharpComposedGaussians>;
    if (delta == nullptr) return ResultT::failure("sharp_compose: null delta");
    if (base.layers < 1 || base.grid <= 0) {
        return ResultT::failure("sharp_compose: the base values are empty");
    }
    const size_t pixels = static_cast<size_t>(base.grid) * base.grid;
    const size_t count = static_cast<size_t>(base.layers) * pixels;
    if (base.mean_x.size() != count || base.color.size() != 3 * count) {
        return ResultT::failure("sharp_compose: the base arrays are inconsistent");
    }

    SharpComposedGaussians composed;
    composed.count = count;
    composed.mean.resize(3 * count);
    composed.singular.resize(3 * count);
    composed.quaternion.resize(4 * count);
    composed.color.resize(3 * count);
    composed.opacity.resize(count);

    const double global_scale = base.global_scale;
    const double scale_a = scale_activation_a();
    const double scale_b = scale_activation_b();
    const float base_opacity = base.opacity;

    for (int layer = 0; layer < base.layers; ++layer) {
        const size_t layer_offset = static_cast<size_t>(layer) * pixels;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            const size_t gaussian = layer_offset + pixel;
            auto delta_of = [&](int attribute) {
                return static_cast<double>(
                    delta[(static_cast<size_t>(attribute) * base.layers + layer) * pixels + pixel]);
            };

            // ---- mean ------------------------------------------------------
            const double xx = static_cast<double>(base.mean_x[gaussian]) + kDeltaXY * delta_of(0);
            const double yy = static_cast<double>(base.mean_y[gaussian]) + kDeltaXY * delta_of(1);
            const double inv_z_base = static_cast<double>(base.inverse_z[gaussian]);
            const double inverse_z =
                softplus(inverse_softplus(inv_z_base) + kDeltaZ * delta_of(2));
            const double z = 1.0 / (inverse_z + 1e-3);
            composed.mean[gaussian * 3 + 0] = static_cast<float>(global_scale * z * xx);
            composed.mean[gaussian * 3 + 1] = static_cast<float>(global_scale * z * yy);
            composed.mean[gaussian * 3 + 2] = static_cast<float>(global_scale * z);

            // ---- scale -----------------------------------------------------
            // models/composer.py:119-130: the base scale is corrected by the
            // predicted z before the relative-scale activation.
            const double base_scale = static_cast<double>(base.scale[gaussian]) * inv_z_base * z;
            for (int axis = 0; axis < 3; ++axis) {
                const double factor =
                    (kMaxScale - kMinScale) *
                        sigmoid(scale_a * kDeltaScale * delta_of(3 + axis) + scale_b) +
                    kMinScale;
                composed.singular[gaussian * 3 + axis] =
                    static_cast<float>(global_scale * base_scale * factor);
            }

            // ---- rotation, colour, opacity ---------------------------------
            for (int component = 0; component < 4; ++component) {
                const double base_value = component == 0 ? 1.0 : 0.0;
                composed.quaternion[gaussian * 4 + component] = static_cast<float>(
                    base_value + kDeltaQuaternion * delta_of(6 + component));
            }
            for (int channel = 0; channel < 3; ++channel) {
                const double clamped = std::min(
                    std::max(static_cast<double>(base.color[static_cast<size_t>(channel) * count +
                                                          gaussian]),
                             kColorMin),
                    kColorMax);
                const double activated =
                    sigmoid(inverse_sigmoid(clamped) + kDeltaColor * delta_of(10 + channel));
                composed.color[gaussian * 3 + channel] =
                    srgb_to_linear(static_cast<float>(activated));
            }
            composed.opacity[gaussian] = static_cast<float>(
                sigmoid(inverse_sigmoid(static_cast<double>(base_opacity)) +
                        kDeltaOpacity * delta_of(13)));
        }
    }
    return ResultT::success(std::move(composed));
}

Result<SplatScene> sharp_unproject(const SharpComposedGaussians& composed, float focal_px,
                                   int width, int height) {
    using ResultT = Result<SplatScene>;
    if (!(focal_px > 0.0f) || width <= 0 || height <= 0) {
        return ResultT::failure("sharp_unproject: invalid intrinsics");
    }
    const size_t count = composed.count;
    if (composed.mean.size() != 3 * count || composed.singular.size() != 3 * count ||
        composed.quaternion.size() != 4 * count || composed.color.size() != 3 * count ||
        composed.opacity.size() != count) {
        return ResultT::failure("sharp_unproject: inconsistent Gaussian arrays");
    }

    SplatScene scene;
    scene.resize(count);
    // M = inv(ndc_matrix @ K_resized) = diag(W/(2 f), H/(2 f), 1).
    const double scale_x = static_cast<double>(width) / (2.0 * focal_px);
    const double scale_y = static_cast<double>(height) / (2.0 * focal_px);

    for (size_t index = 0; index < count; ++index) {
        const float* mean = composed.mean.data() + index * 3;
        // X = M mean with M = diag(W/(2f), H/(2f), 1) (utils/gaussians.py:116).
        // The composer already applied the global scale, so this is the whole
        // unprojection for the means: the anisotropic x/y factors are the NDC
        // stretch, not a change of units.
        scene.position[index * 3 + 0] = static_cast<float>(scale_x * mean[0]);
        scene.position[index * 3 + 1] = static_cast<float>(scale_y * mean[1]);
        scene.position[index * 3 + 2] = mean[2];

        double quaternion[4] = {composed.quaternion[index * 4 + 0],
                                composed.quaternion[index * 4 + 1],
                                composed.quaternion[index * 4 + 2],
                                composed.quaternion[index * 4 + 3]};
        double rotation[3][3];
        rotation_from_quaternion(quaternion, rotation);
        const double sigma[3] = {composed.singular[index * 3 + 0],
                                 composed.singular[index * 3 + 1],
                                 composed.singular[index * 3 + 2]};

        // Sigma = R diag(sigma^2) R^T, then Sigma' = M Sigma M^T with M diagonal.
        const double m[3] = {scale_x, scale_y, 1.0};
        double covariance[3][3];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                double sum = 0.0;
                for (int axis = 0; axis < 3; ++axis) {
                    sum += rotation[row][axis] * sigma[axis] * sigma[axis] * rotation[column][axis];
                }
                covariance[row][column] = m[row] * sum * m[column];
            }
        }

        double eigenvalues[3];
        double eigenvectors[3][3];
        symmetric_eigen(covariance, eigenvalues, eigenvectors);
        for (int axis = 0; axis < 3; ++axis) {
            const double variance = std::max(eigenvalues[axis], 1e-30);
            scene.log_scale[index * 3 + axis] = static_cast<float>(0.5 * std::log(variance));
        }
        float quaternion_out[4];
        quaternion_from_rotation(eigenvectors, quaternion_out);
        std::memcpy(scene.rotation.data() + index * 4, quaternion_out, sizeof(quaternion_out));

        for (int channel = 0; channel < 3; ++channel) {
            const float srgb = linear_to_srgb(composed.color[index * 3 + channel]);
            scene.sh_dc[index * 3 + channel] = (srgb - 0.5f) / kShDcScale;
        }
        // The reference writes +inf when alpha saturates (36 splats in
        // photo01); clamp before the logit so the scene is always finite.
        const float alpha = std::min(std::max(composed.opacity[index], 1e-4f), 1.0f - 1e-4f);
        scene.opacity_logit[index] = std::log(alpha / (1.0f - alpha));
    }
    return ResultT::success(std::move(scene));
}

}  // namespace splat
