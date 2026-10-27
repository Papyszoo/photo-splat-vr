#include "recon/sharp/sharp_preprocess.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace splat {
namespace {

// One axis of the reference's bilinear kernel: the source index, the floor of
// it, the second index (clamped to the last pixel like PyTorch's w1p) and the
// two weights. All float32, as in the CPU kernel.
struct Axis {
    std::vector<int> index0;
    std::vector<int> index1;
    std::vector<float> weight0;
    std::vector<float> weight1;
};

Axis make_axis(int input_size, int output_size, bool align_corners, float scale_hint) {
    Axis axis;
    axis.index0.resize(static_cast<size_t>(output_size));
    axis.index1.resize(static_cast<size_t>(output_size));
    axis.weight0.resize(static_cast<size_t>(output_size));
    axis.weight1.resize(static_cast<size_t>(output_size));
    // area_pixel_compute_scale: (in - 1) / (out - 1) for align_corners, else
    // 1 / scale_factor when the caller supplied one (PyTorch uses the scale
    // factor directly, not out/in), else in / out.
    float scale = 0.0f;
    if (align_corners) {
        scale = static_cast<float>(input_size - 1) / static_cast<float>(output_size - 1);
    } else if (scale_hint > 0.0f) {
        scale = scale_hint;
    } else {
        scale = static_cast<float>(input_size) / static_cast<float>(output_size);
    }
    for (int i = 0; i < output_size; ++i) {
        float source = align_corners ? scale * static_cast<float>(i)
                                     : scale * (static_cast<float>(i) + 0.5f) - 0.5f;
        if (!align_corners && source < 0.0f) source = 0.0f;
        const int base = static_cast<int>(source);
        const int first = std::min(base, input_size - 1);
        const float fraction = source - static_cast<float>(base);
        axis.index0[static_cast<size_t>(i)] = first;
        // PyTorch's h1p/w1p: stay on the last pixel at the border.
        axis.index1[static_cast<size_t>(i)] = first < input_size - 1 ? first + 1 : first;
        axis.weight0[static_cast<size_t>(i)] = 1.0f - fraction;
        axis.weight1[static_cast<size_t>(i)] = fraction;
    }
    return axis;
}

}  // namespace

std::vector<float> sharp_resize_bilinear(const float* src, int width, int height, int channels,
                                         int out_width, int out_height, bool align_corners,
                                         float scale_hint) {
    std::vector<float> out(static_cast<size_t>(channels) * out_width * out_height, 0.0f);
    if (src == nullptr || width <= 0 || height <= 0 || channels <= 0 || out_width <= 0 ||
        out_height <= 0) {
        return out;
    }
    const Axis x_axis = make_axis(width, out_width, align_corners, scale_hint);
    const Axis y_axis = make_axis(height, out_height, align_corners, scale_hint);
    for (int channel = 0; channel < channels; ++channel) {
        const float* plane = src + static_cast<size_t>(channel) * width * height;
        float* destination = out.data() + static_cast<size_t>(channel) * out_width * out_height;
        for (int y = 0; y < out_height; ++y) {
            const float* row0 = plane + static_cast<size_t>(y_axis.index0[static_cast<size_t>(y)]) * width;
            const float* row1 = plane + static_cast<size_t>(y_axis.index1[static_cast<size_t>(y)]) * width;
            const float wy0 = y_axis.weight0[static_cast<size_t>(y)];
            const float wy1 = y_axis.weight1[static_cast<size_t>(y)];
            float* row_out = destination + static_cast<size_t>(y) * out_width;
            for (int x = 0; x < out_width; ++x) {
                const size_t index = static_cast<size_t>(x);
                const int x0 = x_axis.index0[index];
                const int x1 = x_axis.index1[index];
                const float wx0 = x_axis.weight0[index];
                const float wx1 = x_axis.weight1[index];
                const float top = wx0 * row0[x0] + wx1 * row0[x1];
                const float bottom = wx0 * row1[x0] + wx1 * row1[x1];
                row_out[x] = wy0 * top + wy1 * bottom;
            }
        }
    }
    return out;
}

Result<SharpImage> sharp_preprocess(const uint8_t* rgb, int width, int height, int size) {
    using ResultT = Result<SharpImage>;
    if (rgb == nullptr || width <= 0 || height <= 0 || size <= 0) {
        return ResultT::failure("sharp_preprocess: invalid image size");
    }
    // float / 255 in CHW, exactly the tensor the reference resizes.
    std::vector<float> source(static_cast<size_t>(3) * width * height);
    for (int channel = 0; channel < 3; ++channel) {
        float* plane = source.data() + static_cast<size_t>(channel) * width * height;
        for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
            plane[pixel] = static_cast<float>(rgb[pixel * 3 + static_cast<size_t>(channel)]) / 255.0f;
        }
    }
    SharpImage image;
    image.size = size;
    image.chw = sharp_resize_bilinear(source.data(), width, height, 3, size, size,
                                      /*align_corners=*/true, /*scale_hint=*/0.0f);
    return ResultT::success(std::move(image));
}

void sharp_normalise(std::vector<float>* chw) {
    if (chw == nullptr) return;
    for (float& value : *chw) value = 2.0f * value - 1.0f;
}

Result<SharpPatches> sharp_pyramid_patches(const float* chw, int size, int patch_size) {
    using ResultT = Result<SharpPatches>;
    if (chw == nullptr || size <= 0 || patch_size <= 0 || size % patch_size != 0) {
        return ResultT::failure("sharp_pyramid_patches: image size is not a multiple of the patch size");
    }
    // x1 and x2 come straight from x0, as in _create_pyramid (x2 is not built
    // from x1).
    // F.interpolate(scale_factor=s) uses a source step of 1/s, so x1 steps 2 px
    // and x2 steps 4 px in the 1536x1536 image.
    std::vector<float> x1 = sharp_resize_bilinear(chw, size, size, 3, size / 2, size / 2,
                                                 /*align_corners=*/false, /*scale_hint=*/2.0f);
    std::vector<float> x2 = sharp_resize_bilinear(chw, size, size, 3, size / 4, size / 4,
                                                 /*align_corners=*/false, /*scale_hint=*/4.0f);

    SharpPatches patches;
    patches.patch_size = patch_size;
    patches.steps = {size / patch_size, size / (2 * patch_size), 1};

    // split(): patch_stride = int(patch_size * (1 - overlap_ratio)), steps =
    // ceil((image - patch) / stride) + 1, patches emitted row-major.
    struct Level {
        const float* image;
        int image_size;
        double overlap;
        int stride;
        int steps;
    };
    Level levels[] = {
        {chw, size, 0.25, 0, 0},
        {x1.data(), size / 2, 0.5, 0, 0},
        {x2.data(), size / 4, 0.0, 0, 0},
    };
    int total = 0;
    for (Level& level : levels) {
        level.stride = static_cast<int>(static_cast<double>(patch_size) * (1.0 - level.overlap));
        level.steps = static_cast<int>(std::ceil(
                          static_cast<double>(level.image_size - patch_size) / level.stride)) + 1;
        total += level.steps * level.steps;
    }
    patches.count = total;
    patches.steps = {levels[0].steps, levels[1].steps, levels[2].steps};
    patches.values.assign(static_cast<size_t>(total) * 3 * patch_size * patch_size, 0.0f);

    int patch = 0;
    for (const Level& level : levels) {
        const int image_size = level.image_size;
        const int stride = level.stride;
        const int steps = level.steps;
        const float* image = level.image;
        for (int j = 0; j < steps; ++j) {
            const int y0 = j * stride;
            for (int i = 0; i < steps; ++i) {
                const int x0 = i * stride;
                float* out = patches.values.data() +
                             static_cast<size_t>(patch) * 3 * patch_size * patch_size;
                for (int channel = 0; channel < 3; ++channel) {
                    const float* source_plane =
                        image + static_cast<size_t>(channel) * image_size * image_size;
                    float* out_plane = out + static_cast<size_t>(channel) * patch_size * patch_size;
                    for (int y = 0; y < patch_size; ++y) {
                        std::memcpy(out_plane + static_cast<size_t>(y) * patch_size,
                                    source_plane + static_cast<size_t>(y0 + y) * image_size + x0,
                                    static_cast<size_t>(patch_size) * sizeof(float));
                    }
                }
                ++patch;
            }
        }
    }
    // The 384x384 level feeds the image encoder as-is.
    patches.lowres = x2;
    return ResultT::success(std::move(patches));
}

}  // namespace splat
