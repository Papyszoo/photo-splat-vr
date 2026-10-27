#include "recon/ggml/pos_embed.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace splat {
namespace {

constexpr double kCubicA = -0.75;

double cubic_weight(double t) {
    const double a = kCubicA;
    t = std::fabs(t);
    if (t < 1.0) return ((a + 2.0) * t - (a + 3.0)) * t * t + 1.0;
    if (t < 2.0) return ((a * t - 5.0 * a) * t + 8.0 * a) * t - 4.0 * a;
    return 0.0;
}

// Source coordinate for align_corners=False when the scale factor is given.
double source_index(double scale, int destination) {
    return scale * (static_cast<double>(destination) + 0.5) - 0.5;
}

int clamp_index(int value, int size) { return std::min(std::max(value, 0), size - 1); }

}  // namespace

Result<std::vector<float>> resample_pos_embed(const float* pos, int grid_w, int grid_h, int dim,
                                             int out_w, int out_h, float offset) {
    using ResultT = Result<std::vector<float>>;
    if (pos == nullptr || dim <= 0 || grid_w <= 0 || grid_h <= 0 || out_w <= 0 || out_h <= 0) {
        return ResultT::failure("resample_pos_embed: invalid shape");
    }

    std::vector<float> out(static_cast<size_t>(1 + out_w * out_h) * dim, 0.0f);
    std::memcpy(out.data(), pos, static_cast<size_t>(dim) * sizeof(float));  // cls row

    // PyTorch uses 1 / scale_factor as the sampling step (not in/out), and the
    // DINOv2 offset makes the two differ slightly.
    const double step_x = static_cast<double>(grid_w) / (static_cast<double>(out_w) + offset);
    const double step_y = static_cast<double>(grid_h) / (static_cast<double>(out_h) + offset);

    for (int y = 0; y < out_h; ++y) {
        const double src_y = source_index(step_y, y);
        const int base_y = static_cast<int>(std::floor(src_y));
        const double frac_y = src_y - base_y;
        double weights_y[4];
        for (int k = 0; k < 4; ++k) weights_y[k] = cubic_weight(frac_y - (k - 1));

        for (int x = 0; x < out_w; ++x) {
            const double src_x = source_index(step_x, x);
            const int base_x = static_cast<int>(std::floor(src_x));
            const double frac_x = src_x - base_x;

            float* destination =
                out.data() + (static_cast<size_t>(1) + static_cast<size_t>(y) * out_w + x) * dim;
            for (int c = 0; c < dim; ++c) {
                double sum = 0.0;
                for (int kx = 0; kx < 4; ++kx) {
                    const int sx = clamp_index(base_x - 1 + kx, grid_w);
                    const double wx = cubic_weight(frac_x - (kx - 1));
                    double vertical = 0.0;
                    for (int ky = 0; ky < 4; ++ky) {
                        const int sy = clamp_index(base_y - 1 + ky, grid_h);
                        const float* row = pos + (static_cast<size_t>(1) +
                                                  static_cast<size_t>(sy) * grid_w) * dim;
                        vertical += weights_y[ky] * row[static_cast<size_t>(sx) * dim + c];
                    }
                    sum += wx * vertical;
                }
                destination[c] = static_cast<float>(sum);
            }
        }
    }
    return ResultT::success(std::move(out));
}

}  // namespace splat
