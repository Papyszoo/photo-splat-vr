#include "recon/sharp/sharp_initializer.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace splat {
namespace {

// models/initializer.py:281-297. The reference computes the minimum over every
// depth value of the batch (the two layers together), not per layer.
constexpr double kDepthMin = 1.0;
constexpr double kDepthMax = 1e2;
constexpr double kDepthEpsilon = 1e-6;
// InitializerParams defaults that shape the output.
constexpr int kStride = 2;
constexpr double kScaleFactor = 1.0;
// InitializerParams.disparity_factor: "Factor to convert inverse depth to
// disparity", default 1.0. This is the initialiser's own value, unrelated to the
// network conditioning `f_px / width`.
constexpr double kInitializerDisparityFactor = 1.0;

}  // namespace

Result<SharpBaseValues> sharp_initialize(const float* image_chw, int image_size,
                                         const float* monodepth, int layers) {
    using ResultT = Result<SharpBaseValues>;
    if (image_chw == nullptr || monodepth == nullptr) {
        return ResultT::failure("sharp_initialize: null input");
    }
    if (image_size <= 0 || image_size % kStride != 0 || layers < 1) {
        return ResultT::failure("sharp_initialize: image size must be even and positive");
    }
    const int grid = image_size / kStride;
    const size_t pixels = static_cast<size_t>(image_size) * image_size;
    const size_t layer_pixels = static_cast<size_t>(layers) * pixels;
    const size_t gaussians = static_cast<size_t>(layers) * grid * grid;

    // ---- depth normalisation (models/initializer.py:148-150, 281-297) -------
    double minimum = monodepth[0];
    for (size_t index = 1; index < layer_pixels; ++index) {
        minimum = std::min(minimum, static_cast<double>(monodepth[index]));
    }
    const double depth_factor = kDepthMin / (minimum + kDepthEpsilon);
    const double global_scale = 1.0 / depth_factor;

    std::vector<double> depth_norm(layer_pixels);
    for (size_t index = 0; index < layer_pixels; ++index) {
        depth_norm[index] =
            std::min(static_cast<double>(monodepth[index]) * depth_factor, kDepthMax);
    }

    SharpBaseValues base;
    base.image_size = image_size;
    base.grid = grid;
    base.layers = layers;
    base.mean_x.resize(gaussians);
    base.mean_y.resize(gaussians);
    base.inverse_z.resize(gaussians);
    base.scale.resize(gaussians);
    base.color.assign(3 * gaussians, 0.0);
    base.global_scale = static_cast<float>(global_scale);
    base.opacity = static_cast<float>(std::min(1.0 / layers, 0.5));

    // models/initializer.py:210: disparity_scale_factor.
    const double disparity_scale = 2.0 * kScaleFactor * kStride / static_cast<double>(image_size);

    for (int layer = 0; layer < layers; ++layer) {
        const size_t source = static_cast<size_t>(layer) * pixels;
        const size_t target = static_cast<size_t>(layer) * grid * grid;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                // max_pool2d over the 2x2 block; the disparity is 1/depth.
                double disparity = 0.0;
                for (int dy = 0; dy < kStride; ++dy) {
                    for (int dx = 0; dx < kStride; ++dx) {
                        const size_t index =
                            source + static_cast<size_t>(y * kStride + dy) * image_size +
                            (x * kStride + dx);
                        disparity = std::max(disparity, 1.0 / depth_norm[index]);
                    }
                }
                const size_t gaussian = target + static_cast<size_t>(y) * grid + x;
                base.inverse_z[gaussian] = static_cast<float>(disparity);
                base.scale[gaussian] = static_cast<float>((1.0 / disparity) * disparity_scale);
                base.mean_x[gaussian] =
                    static_cast<float>(2.0 * (0.5 * kStride + x * kStride) / image_size - 1.0);
                base.mean_y[gaussian] =
                    static_cast<float>(2.0 * (0.5 * kStride + y * kStride) / image_size - 1.0);
            }
        }
    }

    // ---- colour: avg_pool2d(image, 2, 2), replicated over the layers --------
    for (int channel = 0; channel < 3; ++channel) {
        const float* plane = image_chw + static_cast<size_t>(channel) * pixels;
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                double sum = 0.0;
                for (int dy = 0; dy < kStride; ++dy) {
                    for (int dx = 0; dx < kStride; ++dx) {
                        sum += plane[static_cast<size_t>(y * kStride + dy) * image_size +
                                     (x * kStride + dx)];
                    }
                }
                const float value = static_cast<float>(sum / (kStride * kStride));
                for (int layer = 0; layer < layers; ++layer) {
                    const size_t gaussian = static_cast<size_t>(layer) * grid * grid +
                                            static_cast<size_t>(y) * grid + x;
                    base.color[static_cast<size_t>(channel) * gaussians + gaussian] = value;
                }
            }
        }
    }

    // ---- feature input: 2*cat(image, 1/depth_norm) - 1 ----------------------
    base.feature_input.resize(5 * pixels);
    for (int channel = 0; channel < 3; ++channel) {
        const float* plane = image_chw + static_cast<size_t>(channel) * pixels;
        float* destination = base.feature_input.data() + static_cast<size_t>(channel) * pixels;
        for (size_t index = 0; index < pixels; ++index) {
            destination[index] = static_cast<float>(2.0 * plane[index] - 1.0);
        }
    }
    for (int layer = 0; layer < layers; ++layer) {
        float* destination =
            base.feature_input.data() + static_cast<size_t>(3 + layer) * pixels;
        const double* source = depth_norm.data() + static_cast<size_t>(layer) * pixels;
        for (size_t index = 0; index < pixels; ++index) {
            destination[index] = static_cast<float>(
                2.0 * (kInitializerDisparityFactor / source[index]) - 1.0);
        }
    }
    return ResultT::success(std::move(base));
}

}  // namespace splat
