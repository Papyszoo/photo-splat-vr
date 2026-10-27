#include "scene/psplat.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/half.h"
#include "scene/byte_order.h"

namespace splat {
namespace {

constexpr std::array<uint8_t, 8> kMagic{{'P', 'S', 'P', 'L', 'A', 'T', '\0', '\0'}};

void store_half(uint8_t* p, float value) {
    const uint16_t half = float_to_half(value);
    p[0] = static_cast<uint8_t>(half & 0xffu);
    p[1] = static_cast<uint8_t>(half >> 8);
}

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

template <typename Container>
void write_half_array(std::ostream& out, const Container& values) {
    std::array<uint8_t, 4096> buffer{};
    size_t used = 0;
    for (float value : values) {
        if (used + 2 > buffer.size()) {
            out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(used));
            used = 0;
        }
        store_half(buffer.data() + used, value);
        used += 2;
    }
    if (used > 0) out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(used));
}

}  // namespace

Status write_psplat(const Path& path, const SplatScene& scene, float focal_px,
                    uint32_t image_width, uint32_t image_height) {
    if (!scene.consistent()) return Status::error(".psplat scene arrays have inconsistent sizes");
    if (scene.count() > 0xffffffffu) return Status::error(".psplat count exceeds uint32");

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error("cannot open " + path.string() + " for writing");

    const auto header = make_header(scene, scene.bounds(), focal_px, image_width, image_height);
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    write_half_array(out, scene.position);
    write_half_array(out, scene.log_scale);
    write_half_array(out, scene.rotation);
    write_half_array(out, scene.sh_dc);
    write_half_array(out, scene.opacity_logit);

    if (!out) return Status::error("failed while writing " + path.string());
    return Status::success();
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
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<SplatScene>::failure("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return read_psplat(bytes.data(), bytes.size(), info);
}

}  // namespace splat
