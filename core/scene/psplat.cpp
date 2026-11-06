#include "scene/psplat.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "common/half.h"
#include "scene/byte_order.h"
#include "vfs/io.h"

namespace splat {
namespace {

constexpr std::array<uint8_t, 8> kMagic{{'P', 'S', 'P', 'L', 'A', 'T', '\0', '\0'}};

float load_half(const uint8_t* p) {
    return half_to_float(static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)));
}

std::array<uint8_t, kPsplatHeaderSize> make_header(const SplatScene& scene, const Bounds& bounds,
                                                   float focal_px, uint32_t image_width,
                                                   uint32_t image_height) {
    std::array<uint8_t, kPsplatHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    store_u32(header.data() + 8, kPsplatVersion);
    store_u32(header.data() + 12, static_cast<uint32_t>(scene.count()));
    store_u32(header.data() + 16, SplatScene::kValuesPerSplat);
    store_u32(header.data() + 20, SplatScene::kLayout);
    for (int i = 0; i < 3; ++i) store_f32(header.data() + 24 + i * 4, bounds.min[i]);
    for (int i = 0; i < 3; ++i) store_f32(header.data() + 36 + i * 4, bounds.max[i]);
    store_f32(header.data() + 48, focal_px);
    store_u32(header.data() + 52, image_width);
    store_u32(header.data() + 56, image_height);
    // bytes 60..79 are reserved and stay zero
    return header;
}

void append_half(std::vector<uint8_t>* bytes, float value) {
    const uint16_t half = float_to_half(value);
    bytes->push_back(static_cast<uint8_t>(half & 0xffu));
    bytes->push_back(static_cast<uint8_t>(half >> 8));
}

}  // namespace

Status write_psplat(const Path& path, const SplatScene& scene, float focal_px,
                    uint32_t image_width, uint32_t image_height) {
    if (!scene.consistent()) return Status::error(".psplat scene arrays have inconsistent sizes");
    if (scene.count() > 0xffffffffu) return Status::error(".psplat count exceeds uint32");

    // One buffer, then the VFS write: the .psplat writer is the same code for a
    // local folder and a folder on a network location (L3's upload).
    std::vector<uint8_t> bytes;
    bytes.reserve(psplat_file_size(scene.count()));
    const auto header = make_header(scene, scene.bounds(), focal_px, image_width, image_height);
    bytes.insert(bytes.end(), header.begin(), header.end());
    for (const float value : scene.position) append_half(&bytes, value);
    for (const float value : scene.log_scale) append_half(&bytes, value);
    for (const float value : scene.rotation) append_half(&bytes, value);
    for (const float value : scene.sh_dc) append_half(&bytes, value);
    for (const float value : scene.opacity_logit) append_half(&bytes, value);

    return vfs::write_file(path, bytes);
}

Result<SplatScene> read_psplat(const uint8_t* data, size_t size, PsplatInfo* info) {
    if (data == nullptr || size < kPsplatHeaderSize) {
        return Result<SplatScene>::failure(".psplat is shorter than the 80-byte header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), data)) {
        return Result<SplatScene>::failure(".psplat magic mismatch");
    }

    PsplatInfo parsed;
    parsed.version = load_u32(data + 8);
    parsed.count = load_u32(data + 12);
    parsed.values_per_splat = load_u32(data + 16);
    parsed.layout = load_u32(data + 20);
    for (int i = 0; i < 3; ++i) parsed.bounds_min[i] = load_f32(data + 24 + i * 4);
    for (int i = 0; i < 3; ++i) parsed.bounds_max[i] = load_f32(data + 36 + i * 4);
    parsed.focal_px = load_f32(data + 48);
    parsed.image_width = load_u32(data + 52);
    parsed.image_height = load_u32(data + 56);

    if (parsed.version != kPsplatVersion) {
        return Result<SplatScene>::failure(".psplat version " + std::to_string(parsed.version) +
                                         " is not supported");
    }
    if (parsed.values_per_splat != SplatScene::kValuesPerSplat) {
        return Result<SplatScene>::failure(".psplat has " + std::to_string(parsed.values_per_splat) +
                                         " values per splat, expected 14");
    }
    if (parsed.layout != SplatScene::kLayout) {
        return Result<SplatScene>::failure(".psplat attribute layout " + std::to_string(parsed.layout) +
                                         " is not supported");
    }

    const size_t expected = psplat_file_size(parsed.count);
    if (size != expected) {
        return Result<SplatScene>::failure(".psplat size " + std::to_string(size) + " does not match " +
                                         std::to_string(expected) + " for " +
                                         std::to_string(parsed.count) + " splats");
    }

    SplatScene scene;
    scene.resize(parsed.count);
    const uint8_t* cursor = data + kPsplatHeaderSize;

    auto read_array = [&cursor](std::vector<float>& values) {
        for (float& value : values) {
            value = load_half(cursor);
            cursor += 2;
        }
    };
    read_array(scene.position);
    read_array(scene.log_scale);
    read_array(scene.rotation);
    read_array(scene.sh_dc);
    read_array(scene.opacity_logit);

    if (info != nullptr) *info = parsed;
    return Result<SplatScene>::success(std::move(scene));
}

Result<SplatScene> read_psplat(const Path& path, PsplatInfo* info) {
    const Result<std::vector<uint8_t>> bytes = vfs::read_file(path);
    if (!bytes.ok()) return Result<SplatScene>::failure("cannot open " + path.string());
    return read_psplat(bytes.value().data(), bytes.value().size(), info);
}

}  // namespace splat
