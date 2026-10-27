#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/path.h"
#include "common/status.h"
#include "scene/splat_scene.h"

namespace splat {

// .psplat format version and fixed header size. Header details are documented
// in docs/formats/psplat.md.
inline constexpr uint32_t kPsplatVersion = 1;
inline constexpr size_t kPsplatHeaderSize = 80;
inline constexpr size_t kPsplatBytesPerSplat = SplatScene::kValuesPerSplat * 2;

constexpr size_t psplat_file_size(size_t count) {
    return kPsplatHeaderSize + count * kPsplatBytesPerSplat;
}

struct PsplatInfo {
    uint32_t version = kPsplatVersion;
    uint32_t count = 0;
    uint32_t values_per_splat = SplatScene::kValuesPerSplat;
    uint32_t layout = SplatScene::kLayout;
    std::array<float, 3> bounds_min{{0.0f, 0.0f, 0.0f}};
    std::array<float, 3> bounds_max{{0.0f, 0.0f, 0.0f}};
    float focal_px = 0.0f;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
};

// Writes fp16 SoA arrays. focal_px/image size are optional metadata for the
// renderer/UI; pass zero when unknown. Bounds are computed from the positions.
Status write_psplat(const Path& path, const SplatScene& scene, float focal_px = 0.0f,
                    uint32_t image_width = 0, uint32_t image_height = 0);

// Reads and validates the header. info is filled on success when non-null.
Result<SplatScene> read_psplat(const Path& path, PsplatInfo* info = nullptr);
Result<SplatScene> read_psplat(const uint8_t* data, size_t size, PsplatInfo* info = nullptr);

}  // namespace splat
