// The headerless .splat format (antimatter15): 32-byte records of fp32
// positions, fp32 linear scales, sRGB bytes and quaternion bytes (SPEC
// Appendix A.1).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>

#include "scene/byte_order.h"
#include "splat_import/splat_import.h"

namespace splat {
namespace {

constexpr float kSHC0 = 0.28209479177387814f;

float clamp01_254(uint8_t alpha) {
    const int clamped = alpha < 1 ? 1 : (alpha > 254 ? 254 : alpha);
    return static_cast<float>(clamped) / 255.0f;
}

}  // namespace

Result<SplatScene> import_splat_records(const uint8_t* data, size_t size,
                                        const ImportLimits& limits, ImportReport* report) {
    ImportReport local;
    ImportReport* out = report != nullptr ? report : &local;
    out->format = SplatFormat::splat;

    if (data == nullptr || size == 0 || size % kSplatRecordBytes != 0) {
        return Result<SplatScene>::failure(
            "a .splat file must be a whole number of 32-byte records");
    }
    // The count is the file size divided by the record size, so the guard needs
    // no header: it runs before the scene is sized.
    const uint64_t declared = size / kSplatRecordBytes;
    const Status allowed = check_source_count(declared, limits);
    if (!allowed.ok()) return Result<SplatScene>::failure(allowed.message());

    SplatScene scene;
    scene.resize(static_cast<size_t>(declared));
    for (size_t i = 0; i < declared; ++i) {
        const uint8_t* record = data + i * kSplatRecordBytes;

        for (size_t axis = 0; axis < 3; ++axis) {
            scene.position[i * 3 + axis] = load_f32(record + axis * 4);
            // The stored value is sigma itself, not its logarithm; 1e-9 is the
            // small end fp32 log still resolves.
            const float scale = load_f32(record + 12 + axis * 4);
            scene.log_scale[i * 3 + axis] = std::log(scale > 1e-9f ? scale : 1e-9f);
            scene.sh_dc[i * 3 + axis] =
                (static_cast<float>(record[24 + axis]) / 255.0f - 0.5f) / kSHC0;
        }

        const float alpha = clamp01_254(record[27]);
        scene.opacity_logit[i] = std::log(alpha / (1.0f - alpha));

        // q = (byte - 128) / 128, then normalised: the format quantises a unit
        // quaternion to bytes, so the decoded one is only approximately unit.
        float q[4];
        for (size_t c = 0; c < 4; ++c) {
            q[c] = (static_cast<float>(record[28 + c]) - 128.0f) / 128.0f;
        }
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (length > 1e-6f) {
            for (size_t c = 0; c < 4; ++c) scene.rotation[i * 4 + c] = q[c] / length;
        } else {
            // An all-zero record would otherwise leave a non-unit quaternion
            // that the renderer turns into a degenerate matrix.
            scene.rotation[i * 4 + 0] = 1.0f;
            scene.rotation[i * 4 + 1] = 0.0f;
            scene.rotation[i * 4 + 2] = 0.0f;
            scene.rotation[i * 4 + 3] = 0.0f;
        }
    }

    out->source_count = static_cast<uint32_t>(declared);
    out->kept_count = out->source_count;
    out->note = "splat";
    return Result<SplatScene>::success(std::move(scene));
}

}  // namespace splat
