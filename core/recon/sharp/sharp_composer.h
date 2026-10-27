#pragma once

#include <cstddef>
#include <vector>

#include "common/status.h"
#include "recon/sharp/sharp_initializer.h"
#include "scene/splat_scene.h"

namespace splat {

// C3 stage 6b: the Gaussian composer plus the unprojection.
//
// `sharp_compose` is `GaussianComposer.forward` with the checkpoint's
// parameters (docs/research/sharp-reference.md §9):
//
//   mean   x = x_ndc + 1e-3 dx,  y = y_ndc + 1e-3 dy
//          inv_z = softplus(inverse_softplus(inv_z_ndc) + 1e-3 dz)
//          z = 1/(inv_z + 1e-3),  mean = (z x, z y, z) * global_scale
//   scale  base = scale_ndc * inv_z_ndc * z,
//          sigma = base * 10 sigmoid((10/9) ds + logit(0.1)) * global_scale
//   quat   q = (1, 0, 0, 0) + dq                       (normalised later)
//   colour c = sigmoid(inv_sigmoid(clamp(base, 0.01, 0.99)) + 0.1 dc),
//          then sRGB -> linearRGB
//   alpha  sigmoid(dalpha)
//
// The composer's output stays in the network's normalised space except for
// `global_scale`, which the reference applies here; `sharp_unproject` then
// applies the camera matrix and writes the renderer's layout.
struct SharpComposedGaussians {
    size_t count = 0;
    std::vector<float> mean;        // 3 * count, metres (already global-scaled)
    std::vector<float> singular;    // 3 * count, metres
    std::vector<float> quaternion;  // 4 * count, w x y z, not yet normalised
    std::vector<float> color;       // 3 * count, linearRGB
    std::vector<float> opacity;     // count, alpha in [0,1]
};

// `delta` is the Gaussian decoder's output, torch (14, layers, grid, grid)
// order: index = ((attribute * layers + layer) * grid + y) * grid + x.
Result<SharpComposedGaussians> sharp_compose(const SharpBaseValues& base, const float* delta);

// Unprojection (`utils/gaussians.py:54-131, 172-169`): the reference builds
// M = inv(ndc_matrix @ K_resized), which for a pinhole camera with the internal
// 1536x1536 resolution reduces to diag(W/(2 f), H/(2 f), 1) with the *original*
// width and height, then transforms means and covariances with M and
// re-diagonalises each covariance with an fp64 SVD (descending singular values).
//
// This port computes the same covariance M Sigma M^T in fp64 and diagonalises it
// with a cyclic Jacobi eigensolver; eigenvalues descending is the SVD's singular
// value order, so the scales come out in the reference's order. The quaternion
// is canonicalised to w >= 0: q and -q are the same rotation and scipy's sign is
// unstable (docs/BACKLOG.md), so the output compares by covariance.
//
// Colours are converted linearRGB -> sRGB and packed as SH DC, which is what the
// reference's .ply export does and what .psplat stores.
Result<SplatScene> sharp_unproject(const SharpComposedGaussians& composed, float focal_px,
                                   int width, int height);

// sRGB transfer functions from sharp/utils/color_space.py (the Apple MTL spec
// piecewise curve), exposed for the tests.
float srgb_to_linear(float value);
float linear_to_srgb(float value);

}  // namespace splat
