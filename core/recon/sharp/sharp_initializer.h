#pragma once

#include <vector>

#include "common/status.h"

namespace splat {

// C3 stage 6a: the initialiser (`MultiLayerInitializer`, no parameters).
//
// Everything here is host arithmetic on the monodepth map and the [0,1] image,
// exactly as `models/initializer.py` does it at inference
// (docs/research/sharp-reference.md §9):
//
//   depth_norm    = clamp(depth * depth_factor, max=100),  depth_factor = 1/(min+1e-6)
//   global_scale  = 1/depth_factor = min(depth) + 1e-6
//   inv_z         = max_pool2d(1/depth_norm, 2, 2)          per layer (surface_min)
//   mean_x/y      = 2*(0.5 + k*2)/size - 1                  NDC grid, stride 2
//   scale         = (1/inv_z) * (2*stride/size)
//   colour        = avg_pool2d(image, 2, 2), sRGB [0,1],   same for both layers
//   quaternion    = (1, 0, 0, 0), opacity = min(1/layers, 0.5) = 0.5
//
// `feature_input` is the Gaussian decoder's 5-channel input,
// `2*cat(image, 1/depth_norm) - 1`, in CHW order. Note the *initialiser's* own
// `disparity_factor` (InitializerParams, default 1.0) is what scales the
// inverse-depth channel here, not the network's `f_px / width` conditioning
// value; the two are separate parameters in the reference.
//
// All value arrays are layer-major, `(layer * grid + y) * grid + x`, which is
// the order the golden dumps use. The arrays are replicated over the two layers
// where the reference broadcasts, so they compare to `051_init_base_*` directly.
struct SharpBaseValues {
    int image_size = 0;
    int grid = 0;       // image_size / 2
    int layers = 2;
    std::vector<float> mean_x;      // layers * grid * grid
    std::vector<float> mean_y;
    std::vector<float> inverse_z;
    std::vector<float> scale;
    std::vector<float> color;       // 3 * layers * grid * grid, sRGB
    float quaternion[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float opacity = 0.5f;
    float global_scale = 1.0f;
    std::vector<float> feature_input;  // 5 * image_size^2, CHW
};

// image_chw is the [0,1] network image (stage 0 output), monodepth the metric
// depth at the network resolution (stage 3/4 output), layer-major.
Result<SharpBaseValues> sharp_initialize(const float* image_chw, int image_size,
                                         const float* monodepth, int layers);

}  // namespace splat
