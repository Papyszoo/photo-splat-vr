// Niantic .spz (SPEC Appendix A.2).
//
// v1-v3 are a gzip stream around a 16-byte header and six per-attribute
// streams; v4 is a plaintext 32-byte header, a table of contents and one zstd
// frame per stream. Both end in the same decoder, and the coordinates are
// converted from spz's RUB frame to the scene's OpenCV frame.
//
// The gzip stream is inflated with miniz's tinfl (the vendored copy the core
// already links) only as far as the five streams we keep: a v3 file with SH
// carries 135 bytes per point we drop, and inflating those would cost a
// gigabyte for an 8 M-point scene. The gzip header is skipped here because
// tinfl decodes raw deflate and zlib, not gzip.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <miniz/miniz_tinfl.h>
#if defined(SPLAT_HAVE_ZSTD)
#include <zstd.h>
#endif

#include "common/half.h"
#include "scene/byte_order.h"
#include "splat_import/smallest_three.h"
#include "splat_import/splat_import.h"

namespace splat {
namespace {

constexpr uint32_t kSpzMagic = 0x5053474e;  // "NGSP" little-endian
constexpr size_t kSpzLegacyHeader = 16;
constexpr size_t kSpzV4Header = 32;
constexpr float kColorScale = 0.15f;

struct SpzHeader {
    uint32_t version = 0;
    uint32_t num_points = 0;
    uint8_t sh_degree = 0;
    uint8_t fractional_bits = 0;
    uint8_t flags = 0;
};

uint64_t read_u64(const uint8_t* bytes) {
    return static_cast<uint64_t>(load_u32(bytes)) |
           (static_cast<uint64_t>(load_u32(bytes + 4)) << 32);
}

int32_t read_s24(const uint8_t* bytes) {
    int32_t value = static_cast<int32_t>(bytes[0]) | (static_cast<int32_t>(bytes[1]) << 8) |
                    (static_cast<int32_t>(bytes[2]) << 16);
    if ((value & 0x800000) != 0) value -= 0x1000000;
    return value;
}

// Bytes one point occupies in the five streams we keep.
size_t rotation_bytes(uint32_t version) { return version >= 3 ? 4 : 3; }

// v1 predates the 24-bit fixed-point positions: it stores three fp16 values.
size_t position_bytes(uint32_t version) { return version == 1 ? 6 : 9; }

size_t kept_bytes_per_point(uint32_t version) {
    return position_bytes(version) + 1 + 3 + 3 + rotation_bytes(version);
}

// gzip member header size: the fixed 10 bytes plus the optional fields the FLG
// byte announces. The deflate payload starts at `offset`.
Status gzip_payload_offset(const uint8_t* data, size_t size, size_t* offset) {
    if (size < 18) return Status::error("gzip stream is truncated");
    if (data[0] != 0x1f || data[1] != 0x8b) return Status::error("not a gzip stream");
    if (data[2] != 8) return Status::error("gzip compression method is not deflate");
    const uint8_t flags = data[3];
    size_t at = 10;
    if ((flags & 4) != 0) {  // FEXTRA
        if (at + 2 > size) return Status::error("gzip extra field is truncated");
        const size_t extra = static_cast<size_t>(data[at]) |
                             (static_cast<size_t>(data[at + 1]) << 8);
        at += 2 + extra;
    }
    if ((flags & 8) != 0) {  // FNAME
        while (at < size && data[at] != 0) ++at;
        ++at;
    }
    if ((flags & 16) != 0) {  // FCOMMENT
        while (at < size && data[at] != 0) ++at;
        ++at;
    }
    if ((flags & 2) != 0) at += 2;  // FHCRC
    if (at >= size) return Status::error("gzip header is truncated");
    *offset = at;
    return Status::success();
}

// Inflates a gzip member, stopping as soon as `needed` bytes are available or
// the whole stream ended. `needed` is recomputed after every chunk, so the
// caller can decide from the header that the rest (the SH stream) is not
// wanted. Returns the produced bytes in `out`.
class PrefixInflater {
public:
    PrefixInflater(const uint8_t* source, size_t size, size_t cap)
        : source_(source), size_(size), capacity_(std::max<size_t>(cap, 64)), out_() {
        out_.resize(std::min(capacity_, size_t{1} << 20));
        tinfl_init(&decompressor_);
    }

    // `needed(out)` returns the byte count the caller wants, or 0 while it
    // cannot tell yet (the header is incomplete). True when that much is in.
    template <typename Needed>
    Status run(Needed needed, std::vector<uint8_t>* out) {
        size_t consumed = 0;
        size_t produced = 0;
        for (;;) {
            size_t in_size = size_ - consumed;
            size_t out_size = out_.size() - produced;
            const tinfl_status status =
                tinfl_decompress(&decompressor_, source_ + consumed, &in_size, out_.data(),
                                 out_.data() + produced, &out_size,
                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            consumed += in_size;
            produced += out_size;

            const size_t wanted = needed(out_, produced);
            if (wanted != 0 && produced >= wanted) break;

            if (status == TINFL_STATUS_DONE) {
                return Status::error("gzip stream ended before the declared splat data");
            }
            if (status != TINFL_STATUS_HAS_MORE_OUTPUT) {
                return Status::error("gzip stream is malformed or truncated");
            }
            if (out_.size() >= capacity_) {
                return Status::error("inflated splat data is larger than the import limit allows");
            }
            out_.resize(std::min(capacity_, out_.size() * 2));
        }
        out_.resize(produced);
        *out = std::move(out_);
        return Status::success();
    }

private:
    tinfl_decompressor decompressor_{};
    const uint8_t* source_;
    size_t size_;
    size_t capacity_;
    std::vector<uint8_t> out_;
};

// One point set from the five streams, in spz's own quantisation. `positions`
// etc. point at the start of their stream and hold `num_points` entries.
// Positions and rotations come out in the scene's OpenCV frame.
Result<SplatScene> decode_streams(const SpzHeader& header, const uint8_t* positions,
                                  const uint8_t* alphas, const uint8_t* colours,
                                  const uint8_t* scales, const uint8_t* rotations) {
    const uint32_t count = header.num_points;
    const float fixed_scale = 1.0f / static_cast<float>(1u << (header.fractional_bits & 31u));

    SplatScene scene;
    scene.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        // positions: RUB -> OpenCV. v1 stores three fp16 values, v2+ three
        // signed 24-bit fixed-point values.
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        if (position_bytes(header.version) == 6) {
            const uint8_t* position = positions + static_cast<size_t>(i) * 6;
            px = half_to_float(static_cast<uint16_t>(position[0] | (position[1] << 8)));
            py = half_to_float(static_cast<uint16_t>(position[2] | (position[3] << 8)));
            pz = half_to_float(static_cast<uint16_t>(position[4] | (position[5] << 8)));
        } else {
            const uint8_t* position = positions + static_cast<size_t>(i) * 9;
            px = static_cast<float>(read_s24(position + 0)) * fixed_scale;
            py = static_cast<float>(read_s24(position + 3)) * fixed_scale;
            pz = static_cast<float>(read_s24(position + 6)) * fixed_scale;
        }
        scene.position[i * 3 + 0] = px;
        scene.position[i * 3 + 1] = -py;
        scene.position[i * 3 + 2] = -pz;

        const int clamped = alphas[i] < 1 ? 1 : (alphas[i] > 254 ? 254 : alphas[i]);
        const float alpha = static_cast<float>(clamped) / 255.0f;
        scene.opacity_logit[i] = std::log(alpha / (1.0f - alpha));

        for (size_t channel = 0; channel < 3; ++channel) {
            const float value = static_cast<float>(colours[i * 3 + channel]) / 255.0f;
            scene.sh_dc[i * 3 + channel] = (value - 0.5f) / kColorScale;
        }

        for (size_t axis = 0; axis < 3; ++axis) {
            scene.log_scale[i * 3 + axis] =
                static_cast<float>(scales[i * 3 + axis]) / 16.0f - 10.0f;
        }

        if (rotation_bytes(header.version) == 4) {
            // spz v3+ packs xyzw; the scene stores wxyz.
            float q[4];
            unpack_smallest_three_spz(load_u32(rotations + static_cast<size_t>(i) * 4), q);
            scene.rotation[i * 4 + 0] = q[3];
            scene.rotation[i * 4 + 1] = q[0];
            scene.rotation[i * 4 + 2] = -q[1];
            scene.rotation[i * 4 + 3] = -q[2];
        } else {
            // v1/v2 store xyz in [-1, 1]; w follows from the unit norm.
            const float x = static_cast<float>(rotations[i * 3 + 0]) / 127.5f - 1.0f;
            const float y = static_cast<float>(rotations[i * 3 + 1]) / 127.5f - 1.0f;
            const float z = static_cast<float>(rotations[i * 3 + 2]) / 127.5f - 1.0f;
            const float w = std::sqrt(std::max(0.0f, 1.0f - (x * x + y * y + z * z)));
            scene.rotation[i * 4 + 0] = w;
            scene.rotation[i * 4 + 1] = x;
            scene.rotation[i * 4 + 2] = -y;
            scene.rotation[i * 4 + 3] = -z;
        }
    }
    return Result<SplatScene>::success(std::move(scene));
}

Result<SplatScene> import_spz_legacy(const uint8_t* data, size_t size, const ImportLimits& limits,
                                     ImportReport* out) {
    size_t payload = 0;
    const Status header = gzip_payload_offset(data, size, &payload);
    if (!header.ok()) return Result<SplatScene>::failure(header.message());

    // The inflated prefix never needs more than the header plus the five kept
    // streams of the largest source the limits allow.
    const size_t cap = kSpzLegacyHeader +
                       static_cast<size_t>(limits.max_source_splats) * kept_bytes_per_point(3);
    PrefixInflater inflater(data + payload, size - payload, cap);

    // The header is the first 16 inflated bytes. Once it is in, the count guard
    // runs here, before anything is allocated for the scene, and the inflater
    // stops as soon as the five kept streams are complete (the SH stream a v3
    // file may carry is never inflated).
    SpzHeader parsed;
    std::string failure;
    size_t wanted = 0;
    const auto needed = [&](const std::vector<uint8_t>& bytes, size_t produced) -> size_t {
        if (wanted != 0) return wanted;
        if (produced < kSpzLegacyHeader) return 0;
        if (load_u32(bytes.data()) != kSpzMagic) {
            failure = "gzip stream is not an spz file (no NGSP header)";
            return produced;
        }
        parsed.version = load_u32(bytes.data() + 4);
        parsed.num_points = load_u32(bytes.data() + 8);
        parsed.sh_degree = bytes[12];
        parsed.fractional_bits = bytes[13];
        parsed.flags = bytes[14];
        if (parsed.version < 1 || parsed.version > 3) {
            failure = "spz v" + std::to_string(parsed.version) +
                      " is not a legacy header (v1-v3)";
            return produced;
        }
        const Status allowed = check_source_count(parsed.num_points, limits);
        if (!allowed.ok()) {
            failure = allowed.message();
            return produced;
        }
        wanted = kSpzLegacyHeader + static_cast<size_t>(parsed.num_points) *
                                       kept_bytes_per_point(parsed.version);
        return wanted;
    };

    std::vector<uint8_t> inflated;
    const Status status = inflater.run(needed, &inflated);
    if (!failure.empty()) return Result<SplatScene>::failure(failure);
    if (!status.ok()) return Result<SplatScene>::failure(status.message());

    const uint32_t count = parsed.num_points;
    const uint32_t stream_version = parsed.version;
    const SpzHeader& header_info = parsed;

    const uint8_t* streams = inflated.data() + kSpzLegacyHeader;
    const size_t points = count;
    const uint8_t* positions = streams;
    const uint8_t* alphas = positions + points * position_bytes(stream_version);
    const uint8_t* colours = alphas + points;
    const uint8_t* scales = colours + points * 3;
    const uint8_t* rotations = scales + points * 3;

    Result<SplatScene> scene = decode_streams(header_info, positions, alphas, colours, scales,
                                              rotations);
    if (scene.ok()) {
        out->note = "spz v" + std::to_string(stream_version);
    }
    return scene;
}

Result<SplatScene> import_spz_v4(const uint8_t* data, size_t size, const ImportLimits& limits,
                                 ImportReport* out) {
#if !defined(SPLAT_HAVE_ZSTD)
    (void)data;
    (void)size;
    (void)limits;
    (void)out;
    return Result<SplatScene>::failure(
        "spz v4 is zstd-compressed and this build has no zstd decoder");
#else
    if (size < kSpzV4Header) return Result<SplatScene>::failure("spz v4 header is truncated");
    SpzHeader header;
    header.version = load_u32(data + 4);
    header.num_points = load_u32(data + 8);
    header.sh_degree = data[12];
    header.fractional_bits = data[13];
    header.flags = data[14];
    const uint8_t num_streams = data[15];
    const uint32_t toc = load_u32(data + 16);

    // Count guard before any stream buffer is sized.
    const Status allowed = check_source_count(header.num_points, limits);
    if (!allowed.ok()) return Result<SplatScene>::failure(allowed.message());

    if (num_streams < 5) {
        return Result<SplatScene>::failure("spz v4 declares " + std::to_string(num_streams) +
                                          " streams, the five attribute streams are required");
    }
    if (static_cast<size_t>(toc) + static_cast<size_t>(num_streams) * 16 > size) {
        return Result<SplatScene>::failure("spz v4 table of contents is truncated");
    }

    const size_t count = header.num_points;
    const size_t expected[5] = {count * position_bytes(header.version), count, count * 3,
                                count * 3, count * rotation_bytes(header.version)};
    std::vector<uint8_t> streams[5];
    size_t offset = static_cast<size_t>(toc) + static_cast<size_t>(num_streams) * 16;
    for (size_t index = 0; index < 5; ++index) {
        const uint8_t* entry = data + toc + index * 16;
        const uint64_t compressed = read_u64(entry);
        const uint64_t uncompressed = read_u64(entry + 8);
        if (uncompressed != expected[index]) {
            return Result<SplatScene>::failure(
                "spz v4 stream " + std::to_string(index) + " declares " +
                std::to_string(uncompressed) + " bytes, " + std::to_string(expected[index]) +
                " expected for " + std::to_string(count) + " points");
        }
        // Compare against the bytes that are left, not `offset + compressed`:
        // a TOC entry of 2^64 - 1 wraps that sum and used to reach
        // ZSTD_decompress with a SIZE_MAX source length.
        if (offset > size || compressed > size - offset) {
            return Result<SplatScene>::failure("spz v4 stream " +
                                               std::to_string(index) + " is truncated");
        }
        streams[index].resize(static_cast<size_t>(uncompressed));
        const size_t written =
            ZSTD_decompress(streams[index].data(), streams[index].size(), data + offset,
                            static_cast<size_t>(compressed));
        if (ZSTD_isError(written) != 0) {
            return Result<SplatScene>::failure(
                std::string("spz v4 stream ") + std::to_string(index) + " does not decompress: " +
                ZSTD_getErrorName(written));
        }
        if (written != streams[index].size()) {
            return Result<SplatScene>::failure("spz v4 stream " + std::to_string(index) +
                                               " decompressed to " + std::to_string(written) +
                                               " bytes, " +
                                               std::to_string(streams[index].size()) +
                                               " expected");
        }
        offset += static_cast<size_t>(compressed);
    }

    Result<SplatScene> scene =
        decode_streams(header, streams[0].data(), streams[1].data(), streams[2].data(),
                       streams[3].data(), streams[4].data());
    if (scene.ok()) out->note = "spz v4";
    return scene;
#endif
}

}  // namespace

Result<SplatScene> import_spz(const uint8_t* data, size_t size, const ImportLimits& limits,
                              ImportReport* report) {
    ImportReport local;
    ImportReport* out = report != nullptr ? report : &local;
    out->format = SplatFormat::spz;

    if (data == nullptr || size < 4) {
        return Result<SplatScene>::failure("spz file is empty or truncated");
    }
    if (data[0] == 0x1f && data[1] == 0x8b) {
        const Result<SplatScene> scene = import_spz_legacy(data, size, limits, out);
        if (!scene.ok()) {
            out->note.clear();
            return scene;
        }
        out->source_count = static_cast<uint32_t>(scene.value().count());
        out->kept_count = out->source_count;
        return scene;
    }
    if (load_u32(data) != kSpzMagic) {
        return Result<SplatScene>::failure("spz file has no NGSP header and is not gzip");
    }
    // The version sits at byte 4, so the 4-byte check above is not enough: a
    // 4-to-7-byte file that starts "NGSP" is a truncated download, and reading
    // the version there would read past the buffer.
    if (size < 8) return Result<SplatScene>::failure("spz header is truncated");
    const uint32_t version = load_u32(data + 4);
    if (version >= 4) {
        const Result<SplatScene> scene = import_spz_v4(data, size, limits, out);
        if (!scene.ok()) {
            out->note.clear();
            return scene;
        }
        out->source_count = static_cast<uint32_t>(scene.value().count());
        out->kept_count = out->source_count;
        return scene;
    }
    // A plaintext v1-v3 header: the same streams, uncompressed.
    if (size < kSpzLegacyHeader) return Result<SplatScene>::failure("spz header is truncated");
    const Status allowed = check_source_count(load_u32(data + 8), limits);
    if (!allowed.ok()) return Result<SplatScene>::failure(allowed.message());
    SpzHeader legacy;
    legacy.version = version;
    legacy.num_points = load_u32(data + 8);
    legacy.sh_degree = data[12];
    legacy.fractional_bits = data[13];
    legacy.flags = data[14];
    const size_t points = legacy.num_points;
    const size_t needed = kSpzLegacyHeader + points * kept_bytes_per_point(legacy.version);
    if (size < needed) return Result<SplatScene>::failure("spz stream data is truncated");
    const uint8_t* streams = data + kSpzLegacyHeader;
    const uint8_t* alphas = streams + points * position_bytes(legacy.version);
    Result<SplatScene> scene = decode_streams(legacy, streams, alphas, alphas + points,
                                              alphas + points * 4, alphas + points * 7);
    if (!scene.ok()) return scene;
    out->note = "spz v" + std::to_string(legacy.version);
    out->source_count = static_cast<uint32_t>(scene.value().count());
    out->kept_count = out->source_count;
    return scene;
}

}  // namespace splat
