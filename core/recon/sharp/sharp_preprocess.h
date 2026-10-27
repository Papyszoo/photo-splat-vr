#pragma once

#include <cstdint>
#include <vector>

#include "common/status.h"

namespace splat {

// C3 stage 0: SHARP's preprocessing, as the reference does it
// (docs/research/sharp-reference.md §5):
//
//   decode -> float/255 -> F.interpolate(size=(1536,1536), mode="bilinear",
//   align_corners=True)                       -> 011_preprocess_out, [0,1]
//   AffineRangeNormalizer((0,1) -> (-1,1)), 2x - 1 -> 020_encoder_in
//
// The resize is anisotropic: no letterbox, no crop, align_corners=True. It
// looks wrong but the unprojection scales the intrinsics by 1536/width, so the
// stretch cancels. Do not "fix" it.
//
// Everything is done in float32 with PyTorch's kernel arithmetic (float scale,
// float source index, vertical combine last), which is what keeps the stage
// within a couple of ulp of the CPU golden.

struct SharpImage {
    int size = 0;              // 1536
    std::vector<float> chw;    // 3 * size * size, [0,1], x fastest
};

// rgb is tightly packed RGB8, width * height * 3 bytes.
Result<SharpImage> sharp_preprocess(const uint8_t* rgb, int width, int height, int size = 1536);

// In-place 2x - 1, the encoder's input normalisation.
void sharp_normalise(std::vector<float>* chw);

// 3-level pyramid (1536, 768, 384) plus the 35 sliding-window patches the
// patch encoder runs on, identical to spn_encoder.split(_create_pyramid):
//
//   x0 = x                          -> 5x5 patches, overlap 0.25 (stride 288)
//   x1 = interpolate(x, 0.5)        -> 3x3 patches, overlap 0.5  (stride 192)
//   x2 = interpolate(x, 0.25)       -> the whole 384x384 level
//
// `chw` must be the *normalised* encoder input (that is what the pyramid is
// built from in the reference; bilinear is linear so the order only changes
// rounding). Patch order matches the reference: x0 row-major, then x1, then x2.
struct SharpPatches {
    int patch_size = 384;
    int count = 0;                  // 35
    std::vector<int> steps;         // {5, 3, 1} patches per side
    std::vector<float> values;      // count * 3 * patch_size^2, x fastest per patch
    // The 384x384 low-resolution level, which also feeds the image encoder.
    std::vector<float> lowres;      // 3 * 384 * 384
};

Result<SharpPatches> sharp_pyramid_patches(const float* chw, int size,
                                           int patch_size);

// F.interpolate(mode="bilinear") for one CHW image, in float32. `scale_hint` is
// the source step (1/scale_factor) PyTorch uses for align_corners=False; pass 0
// for align_corners=True. Exposed for the tests' pyramid check.
std::vector<float> sharp_resize_bilinear(const float* src, int width, int height, int channels,
                                         int out_width, int out_height, bool align_corners,
                                         float scale_hint);

}  // namespace splat
