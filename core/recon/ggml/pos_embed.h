#pragma once

#include <cstdint>
#include <vector>

#include "common/status.h"

namespace splat {

// Bicubic resampling of a ViT position embedding, matching the reference:
//
//   F.interpolate(pos, scale_factor=(sx, sy), mode="bicubic",
//                 align_corners=False, antialias=False)
//
// with sx = (out_w + offset) / grid_w and sy = (out_h + offset) / grid_h
// (DINOv2 passes interpolate_offset=0.1; the cubic coefficient is a = -0.75).
// PyTorch uses the given scale factor directly in the kernel, not out/in, which
// is why the offset changes the sample positions at all.
//
// `pos` is (1 + grid_w*grid_h) rows of `dim` floats, row 0 the cls token.
// Returns (1 + out_w*out_h) rows of `dim` floats: the cls row copied, then the
// resampled patch grid in row-major order (index = y * out_w + x).
Result<std::vector<float>> resample_pos_embed(const float* pos, int grid_w, int grid_h, int dim,
                                             int out_w, int out_h, float offset = 0.1f);

}  // namespace splat
