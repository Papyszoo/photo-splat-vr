// SPDX-License-Identifier: GPL-3.0-or-later

#include "scene/imported_scene.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "common/image.h"
#include "jobs/image_io.h"
#include "scene/psplat.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr float kSHC0 = 0.28209479177387814f;
constexpr int kThumbQuality = 90;

// The gallery tile's colour: the scene's mean sh_dc, back in 0..255 sRGB.
std::array<float, 3> mean_colour(const SplatScene& scene) {
    double sums[3] = {0.0, 0.0, 0.0};
    const size_t count = scene.count();
    for (size_t i = 0; i < count; ++i) {
        for (size_t channel = 0; channel < 3; ++channel) {
            sums[channel] += scene.sh_dc[i * 3 + channel];
        }
    }
    std::array<float, 3> colour{{0.5f, 0.5f, 0.5f}};
    if (count == 0) return colour;
    for (size_t channel = 0; channel < 3; ++channel) {
        const float sh = static_cast<float>(sums[channel] / static_cast<double>(count));
        colour[channel] = 0.5f + sh * kSHC0;  // 0..1 linear-ish sRGB
    }
    return colour;
}

uint8_t to_byte(float value) {
    const float scaled = value * 255.0f;
    if (!(scaled > 0.0f)) return 0;  // also catches NaN
    if (scaled >= 255.0f) return 255;
    return static_cast<uint8_t>(scaled + 0.5f);
}

}  // namespace

Status write_imported_scene(const Path& folder, const SplatScene& scene, const SceneMeta& meta,
                            const std::vector<uint8_t>& thumb_jpeg) {
    if (thumb_jpeg.empty()) return Status::error("imported scene needs a non-empty thumb.jpg");

    const Status made = vfs::make_dirs(folder);
    if (!made.ok()) return made;

    const SceneFiles files = scene_files(folder);
    Status status = write_bytes_file(files.thumbnail, thumb_jpeg);
    if (!status.ok()) return status;
    status = write_psplat(files.splat, scene, meta.intrinsics.fx, meta.intrinsics.width,
                          meta.intrinsics.height);
    if (!status.ok()) return status;

    SceneMeta stored = meta;
    stored.splat_count = static_cast<uint32_t>(scene.count());
    return write_scene_meta(files.meta, stored);
}

std::vector<uint8_t> make_import_thumb(const SplatScene& scene, int width, int height) {
    if (width <= 0 || height <= 0 || scene.empty()) return {};

    const std::array<float, 3> colour = mean_colour(scene);
    Image image;
    image.resize(width, height);
    for (int y = 0; y < height; ++y) {
        // 0.94 at the top, 1.06 at the bottom: a 12 % gradient whose mean is
        // exactly the scene colour, which is what the thumbnail test checks.
        const float t = height > 1 ? static_cast<float>(y) / static_cast<float>(height - 1) : 0.5f;
        const float factor = 0.94f + 0.12f * t;
        for (int x = 0; x < width; ++x) {
            uint8_t* pixel = image.pixel(x, y);
            for (size_t channel = 0; channel < 3; ++channel) {
                pixel[channel] = to_byte(colour[channel] * factor);
            }
        }
    }
    const Result<std::vector<uint8_t>> encoded = encode_jpeg(image, kThumbQuality);
    if (!encoded.ok()) return {};
    return encoded.value();
}

}  // namespace splat
