// The four splat readers, on hand-built byte vectors: .splat records, an spz v3
// legacy gzip stream, an spz v4 zstd/RLE-free frame set, a SuperSplat
// .compressed.ply in both chunk variants, the format sniff and the guards.
//
// Every vector is written here, so nothing depends on a fixture on this host.
// tests/fixtures/splats/README.md says where real files would go.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <miniz/miniz.h>
#include <miniz/miniz_tdef.h>

#include "common/half.h"
#include "common/path.h"
#include "scene/psplat.h"
#include "scene/scene_folder.h"
#include "splat_import/splat_format.h"
#include "splat_import/splat_import.h"
#include "data_dir.h"
#include "test_util.h"

using splat::ImportLimits;
using splat::ImportReport;
using splat::SplatFormat;
using splat::SplatScene;

namespace {

constexpr float kSHC0 = 0.28209479177387814f;

void put_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

void put_u64(std::vector<uint8_t>& bytes, uint64_t value) {
    put_u32(bytes, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    put_u32(bytes, static_cast<uint32_t>(value >> 32));
}

void put_f32(std::vector<uint8_t>& bytes, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(bytes, bits);
}

void put_s24(std::vector<uint8_t>& bytes, int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value) & 0xFFFFFFu;
    bytes.push_back(static_cast<uint8_t>(bits & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 8) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((bits >> 16) & 0xFFu));
}

void put_half(std::vector<uint8_t>& bytes, float value) {
    const uint16_t half = splat::float_to_half(value);
    bytes.push_back(static_cast<uint8_t>(half & 0xFFu));
    bytes.push_back(static_cast<uint8_t>(half >> 8));
}

void put_text(std::vector<uint8_t>& bytes, const std::string& text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}

// A .splat record with the values the L2-A test pins (Appendix A.1).
void put_splat_record(std::vector<uint8_t>& bytes, float x, float y, float z, float sx, float sy,
                      float sz, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t qw, uint8_t qx,
                      uint8_t qy, uint8_t qz) {
    put_f32(bytes, x);
    put_f32(bytes, y);
    put_f32(bytes, z);
    put_f32(bytes, sx);
    put_f32(bytes, sy);
    put_f32(bytes, sz);
    bytes.push_back(r);
    bytes.push_back(g);
    bytes.push_back(b);
    bytes.push_back(a);
    bytes.push_back(qw);
    bytes.push_back(qx);
    bytes.push_back(qy);
    bytes.push_back(qz);
}

std::vector<uint8_t> two_splat_file() {
    std::vector<uint8_t> bytes;
    put_splat_record(bytes, 0.0f, 0.0f, 1.0f, 0.01f, 0.02f, 0.03f, 255, 0, 0, 128, 128, 128, 128,
                     255);
    put_splat_record(bytes, 1.0f, 0.0f, 2.0f, 0.01f, 0.02f, 0.03f, 255, 0, 0, 128, 128, 128, 128,
                     255);
    return bytes;
}

// gzip around a raw deflate stream: miniz's tdefl writes deflate, not gzip.
std::vector<uint8_t> gzip(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xFF};
    size_t deflated_size = 0;
    void* deflated = tdefl_compress_mem_to_heap(payload.data(), payload.size(), &deflated_size, 0);
    REQUIRE(deflated != nullptr);
    out.insert(out.end(), static_cast<const uint8_t*>(deflated),
               static_cast<const uint8_t*>(deflated) + deflated_size);
    std::free(deflated);
    put_u32(out, static_cast<uint32_t>(mz_crc32(0, payload.data(), payload.size())));
    put_u32(out, static_cast<uint32_t>(payload.size()));
    return out;
}

// The uncompressed spz v3 legacy body: header, positions, alphas, colours,
// scales, rotations, SH (degree 0, none).
std::vector<uint8_t> spz_v3_body(int32_t x, int32_t y, int32_t z, uint8_t alpha, uint8_t colour,
                                 uint8_t scale, uint32_t rotation) {
    std::vector<uint8_t> body;
    put_u32(body, 0x5053474eu);  // "NGSP"
    put_u32(body, 3);            // version
    put_u32(body, 1);            // numPoints
    body.push_back(0);           // shDegree
    body.push_back(12);          // fractionalBits
    body.push_back(0);           // flags
    body.push_back(0);           // reserved
    put_s24(body, x);
    put_s24(body, y);
    put_s24(body, z);
    body.push_back(alpha);
    body.push_back(colour);
    body.push_back(colour);
    body.push_back(colour);
    body.push_back(scale);
    body.push_back(scale);
    body.push_back(scale);
    put_u32(body, rotation);
    return body;
}

// The uncompressed spz v1 legacy body: the same header and stream order, but
// positions are three fp16 values per point (load-spz.cc: version 1).
std::vector<uint8_t> spz_v1_body(float x, float y, float z, uint8_t alpha, uint8_t colour,
                                 uint8_t scale, uint8_t qx, uint8_t qy, uint8_t qz) {
    std::vector<uint8_t> body;
    put_u32(body, 0x5053474eu);  // "NGSP"
    put_u32(body, 1);            // version
    put_u32(body, 1);            // numPoints
    body.push_back(0);           // shDegree
    body.push_back(12);          // fractionalBits (unused by fp16 positions)
    body.push_back(0);           // flags
    body.push_back(0);           // reserved
    put_half(body, x);
    put_half(body, y);
    put_half(body, z);
    body.push_back(alpha);
    body.push_back(colour);
    body.push_back(colour);
    body.push_back(colour);
    body.push_back(scale);
    body.push_back(scale);
    body.push_back(scale);
    body.push_back(qx);
    body.push_back(qy);
    body.push_back(qz);
    return body;
}

// A zstd frame holding one raw block: magic, frame header descriptor (no
// content size, no checksum), a 256 KiB window, then the block header and the
// bytes. The test has no zstd compressor, and the reader only has to decode.
std::vector<uint8_t> zstd_raw_frame(const std::vector<uint8_t>& payload) {
    REQUIRE(payload.size() < 128 * 1024);
    std::vector<uint8_t> frame;
    put_u32(frame, 0xFD2FB528u);
    frame.push_back(0x00);  // frame header descriptor: no content size, no checksum
    frame.push_back(0x40);  // window descriptor: exponent 8 (256 KiB)
    const uint32_t block = 1u /*last*/ | (0u << 1) /*raw*/ |
                           (static_cast<uint32_t>(payload.size()) << 3);
    frame.push_back(static_cast<uint8_t>(block & 0xFFu));
    frame.push_back(static_cast<uint8_t>((block >> 8) & 0xFFu));
    frame.push_back(static_cast<uint8_t>((block >> 16) & 0xFFu));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

std::string ply_header(size_t chunks, size_t vertices, bool colour_range) {
    std::string header = "ply\nformat binary_little_endian 1.0\ncomment compressed test\n";
    header += "element chunk " + std::to_string(chunks) + "\n";
    for (const char* name : {"min_x", "min_y", "min_z", "max_x", "max_y", "max_z"}) {
        header += std::string("property float ") + name + "\n";
    }
    for (const char* name : {"min_scale_x", "min_scale_y", "min_scale_z", "max_scale_x",
                             "max_scale_y", "max_scale_z"}) {
        header += std::string("property float ") + name + "\n";
    }
    if (colour_range) {
        for (const char* name : {"min_r", "min_g", "min_b", "max_r", "max_g", "max_b"}) {
            header += std::string("property float ") + name + "\n";
        }
    }
    header += "element vertex " + std::to_string(vertices) + "\n";
    header += "property uint packed_position\n";
    header += "property uint packed_rotation\n";
    header += "property uint packed_scale\n";
    header += "property uint packed_color\n";
    header += "end_header\n";
    return header;
}

// 11/10/11-bit quantisation, the packing Appendix A.3 describes.
uint32_t pack_position(uint32_t x, uint32_t y, uint32_t z) {
    return (x << 21) | (y << 11) | z;
}

// .compressed.ply smallest-three: the largest component (w) at index 0 and
// three centred 10-bit fields; fields at 511 encode ~0.
uint32_t pack_rotation_identity_compressed_ply() { return (511u << 20) | (511u << 10) | 511u; }

// spz v3+ smallest-three: the largest component (w) is index 3 of xyzw, and the
// other three are 9-bit magnitudes with a sign bit. Identity is largest = 3 and
// three zero magnitudes.
constexpr uint32_t kSpzIdentityRotation = 3u << 30;

// The magnitude a component is stored as, so a test can compare against the
// quantised value instead of the float it asked for.
float spz_quantised(float value) {
    const uint32_t mag = static_cast<uint32_t>(
        std::fabs(value) / 0.70710678118654752440f * 511.0f + 0.5f);
    return static_cast<float>(mag) / 511.0f * 0.70710678118654752440f;
}

// spz packs the three non-largest fields into the low 30 bits, lowest field
// first, walking xyzw from index 3 down and skipping the largest. With w (3)
// largest: bits 0-9 -> z, 10-19 -> y, 20-29 -> x, each `mag | sign << 9`.
uint32_t pack_spz_rotation(float x, float y, float z) {
    const auto field = [](float value) {
        const uint32_t mag = static_cast<uint32_t>(
            std::fabs(value) / 0.70710678118654752440f * 511.0f + 0.5f);
        return mag | (value < 0.0f ? 0x200u : 0u);
    };
    return kSpzIdentityRotation | (field(x) << 20) | (field(y) << 10) | field(z);
}

std::vector<uint8_t> compressed_ply(bool colour_range) {
    std::vector<uint8_t> file;
    put_text(file, ply_header(1, 2, colour_range));

    // One chunk: positions 0..2 on x, -1..1 on y, 10..12 on z; scales -6..-2 on
    // each axis; colours 0..1 when the range is present.
    put_f32(file, 0.0f);   // min_x
    put_f32(file, -1.0f);  // min_y
    put_f32(file, 10.0f);  // min_z
    put_f32(file, 2.0f);   // max_x
    put_f32(file, 1.0f);   // max_y
    put_f32(file, 12.0f);  // max_z
    for (int axis = 0; axis < 3; ++axis) put_f32(file, -6.0f);  // min_scale_*
    for (int axis = 0; axis < 3; ++axis) put_f32(file, -2.0f);  // max_scale_*
    if (colour_range) {
        for (int channel = 0; channel < 3; ++channel) put_f32(file, 0.0f);  // min_*
        for (int channel = 0; channel < 3; ++channel) put_f32(file, 1.0f);  // max_*
    }

    // Splat 0: x = 0 (0/2047), y = 0 (0/1023), z = 0; scale all minimum.
    put_u32(file, pack_position(0, 0, 0));
    put_u32(file, pack_rotation_identity_compressed_ply());
    put_u32(file, pack_position(0, 0, 0));
    put_u32(file, 0xFFFFFFFFu);  // r=g=b=a=255

    // Splat 1: x = 2047 (2.0), y = 1023 (1.0), z = 2047 (12.0); scale maximum.
    put_u32(file, pack_position(2047, 1023, 2047));
    put_u32(file, pack_rotation_identity_compressed_ply());
    put_u32(file, pack_position(2047, 1023, 2047));
    put_u32(file, 0x80808080u);  // every byte 128, every alpha 128
    return file;
}

}  // namespace

TEST_CASE(".splat records decode to the standard attributes") {
    const std::vector<uint8_t> bytes = two_splat_file();
    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> scene = splat::import_splat(
        splat::Path("robot.splat"), bytes.data(), bytes.size(), limits, &report);
    REQUIRE_MESSAGE(scene.ok(), scene.message());
    CHECK(report.format == SplatFormat::splat);
    CHECK(report.source_count == 2);
    CHECK(report.kept_count == 2);
    CHECK(!report.thinned);

    const SplatScene& loaded = scene.value();
    REQUIRE(loaded.count() == 2);
    CHECK(loaded.position[0] == doctest::Approx(0.0f));
    CHECK(loaded.position[2] == doctest::Approx(1.0f));
    CHECK(loaded.position[3] == doctest::Approx(1.0f));
    CHECK(loaded.position[5] == doctest::Approx(2.0f));

    // sh_dc = (c/255 - 0.5) / 0.2820948: red 255 -> +1.772454, green 0 -> -1.772454.
    CHECK(loaded.sh_dc[0] == doctest::Approx(1.7724539f).epsilon(1e-5));
    CHECK(loaded.sh_dc[1] == doctest::Approx(-1.7724539f).epsilon(1e-5));
    CHECK(loaded.sh_dc[2] == doctest::Approx(-1.7724539f).epsilon(1e-5));

    // log_scale = ln(scale): ln(0.01) = -4.60517, ln(0.02), ln(0.03).
    CHECK(loaded.log_scale[0] == doctest::Approx(std::log(0.01f)).epsilon(1e-5));
    CHECK(loaded.log_scale[1] == doctest::Approx(std::log(0.02f)).epsilon(1e-5));
    CHECK(loaded.log_scale[2] == doctest::Approx(std::log(0.03f)).epsilon(1e-5));

    // Alpha 128 -> ln(128/127) = 0.007843, i.e. zero within a hundredth.
    CHECK(loaded.opacity_logit[0] == doctest::Approx(std::log(128.0f / 127.0f)).epsilon(1e-5));
    CHECK(std::abs(loaded.opacity_logit[0]) < 0.01f);

    // Bytes (128,128,128,255) are (0,0,0,0.9921875) before normalisation, so the
    // stored quaternion comes out as +Z with a unit length.
    REQUIRE(loaded.rotation.size() == 8);
    CHECK(loaded.rotation[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(loaded.rotation[1] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(loaded.rotation[2] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(loaded.rotation[3] == doctest::Approx(1.0f).epsilon(1e-6));
    const float length = std::sqrt(loaded.rotation[0] * loaded.rotation[0] +
                                   loaded.rotation[1] * loaded.rotation[1] +
                                   loaded.rotation[2] * loaded.rotation[2] +
                                   loaded.rotation[3] * loaded.rotation[3]);
    CHECK(length == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("spz v3 legacy stream decodes and flips RUB to OpenCV") {
    // Position (0,0,1) at fractionalBits 12; colour byte 128; scale byte 160;
    // alpha 128; smallest-three identity (largest = w at xyzw index 3, zero
    // magnitudes).
    const std::vector<uint8_t> body =
        spz_v3_body(0, 0, 1 << 12, 128, 128, 160, kSpzIdentityRotation);
    const std::vector<uint8_t> bytes = gzip(body);
    CHECK(bytes[0] == 0x1f);
    CHECK(bytes[1] == 0x8b);

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_splat(splat::Path("robot.spz"), bytes.data(), bytes.size(), limits, &report);
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    CHECK(report.format == SplatFormat::spz);
    CHECK(report.note == "spz v3");
    const SplatScene& scene = imported.value();
    REQUIRE(scene.count() == 1);

    // spz is right/up/back; the scene is right/down/forward.
    CHECK(scene.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(scene.position[1] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(scene.position[2] == doctest::Approx(-1.0f).epsilon(1e-6));

    // sh_dc = (128/255 - 0.5) / 0.15 = 0.01307.
    CHECK(scene.sh_dc[0] == doctest::Approx((128.0f / 255.0f - 0.5f) / 0.15f).epsilon(1e-5));
    CHECK(scene.sh_dc[0] == doctest::Approx(0.01307f).epsilon(1e-3));
    // log_scale = 160/16 - 10 = 0.
    CHECK(scene.log_scale[0] == doctest::Approx(0.0f).epsilon(1e-6));
    // alpha 128 -> ln(128/127) = 0.0078, and the clamp keeps it finite.
    CHECK(scene.opacity_logit[0] == doctest::Approx(std::log(128.0f / 127.0f)).epsilon(1e-5));
    // Identity quaternion within the 10-bit quantisation, and unit length.
    CHECK(scene.rotation[0] == doctest::Approx(1.0f).epsilon(2e-3));
    CHECK(std::abs(scene.rotation[1]) < 1e-2f);
    CHECK(std::abs(scene.rotation[2]) < 1e-2f);
    CHECK(std::abs(scene.rotation[3]) < 1e-2f);
}

TEST_CASE("spz v4 zstd frames decode the same streams") {
    // One point: position (0,1,0) at fractionalBits 12, colour 255, scale 192
    // (log_scale 2), alpha 254, largest-three identity.
    std::vector<uint8_t> positions;
    put_s24(positions, 0);
    put_s24(positions, 1 << 12);
    put_s24(positions, 0);
    const std::vector<uint8_t> alphas = {254};
    const std::vector<uint8_t> colours = {255, 255, 255};
    std::vector<uint8_t> scales;
    for (int axis = 0; axis < 3; ++axis) scales.push_back(192);
    std::vector<uint8_t> rotations;
    put_u32(rotations, kSpzIdentityRotation);
    const std::vector<std::vector<uint8_t>> streams = {positions, alphas, colours, scales,
                                                       rotations};

    std::vector<uint8_t> header;
    put_u32(header, 0x5053474eu);  // NGSP
    put_u32(header, 4);            // version
    put_u32(header, 1);            // numPoints
    header.push_back(0);           // shDegree
    header.push_back(12);          // fractionalBits
    header.push_back(0);           // flags
    header.push_back(5);           // numStreams
    put_u32(header, 0);            // tocByteOffset, patched below
    for (int i = 0; i < 12; ++i) header.push_back(0);
    REQUIRE(header.size() == 32);

    // The TOC follows the extension records; here there are none, so it comes
    // right after the 32-byte header, and the frames follow it.
    std::vector<uint8_t> frames;
    std::vector<uint8_t> toc;
    for (const std::vector<uint8_t>& stream : streams) {
        const std::vector<uint8_t> frame = zstd_raw_frame(stream);
        put_u64(toc, frame.size());
        put_u64(toc, stream.size());
        frames.insert(frames.end(), frame.begin(), frame.end());
    }
    std::vector<uint8_t> bytes = header;
    const uint32_t toc_offset = static_cast<uint32_t>(bytes.size());
    bytes.insert(bytes.end(), toc.begin(), toc.end());
    bytes.insert(bytes.end(), frames.begin(), frames.end());
    // Patch the TOC offset into the header.
    for (size_t i = 0; i < 4; ++i) {
        bytes[16 + i] = static_cast<uint8_t>((toc_offset >> (8 * i)) & 0xFFu);
    }

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_spz(bytes.data(), bytes.size(), limits, &report);
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    CHECK(report.note == "spz v4");
    const SplatScene& scene = imported.value();
    REQUIRE(scene.count() == 1);
    // (0,1,0) in RUB is (0,-1,0) in OpenCV.
    CHECK(scene.position[1] == doctest::Approx(-1.0f).epsilon(1e-6));
    CHECK(scene.sh_dc[0] == doctest::Approx((1.0f - 0.5f) / 0.15f).epsilon(1e-5));
    CHECK(scene.log_scale[0] == doctest::Approx(2.0f).epsilon(1e-6));
    CHECK(scene.rotation[0] == doctest::Approx(1.0f).epsilon(2e-3));
}

TEST_CASE("spz v3 smallest-three decodes xyzw and flips to the scene's wxyz") {
    // x = +0.5, y = -0.5, z = 0, w reconstructed. The format stores xyzw and the
    // scene stores wxyz with the RUB -> OpenCV flip (x, -y, -z), so y comes out
    // positive again.
    const float qx = spz_quantised(0.5f);
    const float qy = -spz_quantised(0.5f);
    const float qw = std::sqrt(1.0f - qx * qx - qy * qy);
    const std::vector<uint8_t> body =
        spz_v3_body(0, 0, 0, 128, 128, 160, pack_spz_rotation(0.5f, -0.5f, 0.0f));
    const std::vector<uint8_t> bytes = gzip(body);

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_spz(bytes.data(), bytes.size(), limits, &report);
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    const SplatScene& scene = imported.value();
    REQUIRE(scene.count() == 1);
    CHECK(scene.rotation[0] == doctest::Approx(qw).epsilon(1e-5));
    CHECK(scene.rotation[1] == doctest::Approx(qx).epsilon(1e-5));
    CHECK(scene.rotation[2] == doctest::Approx(-qy).epsilon(1e-5));
    CHECK(scene.rotation[3] == doctest::Approx(0.0f).epsilon(1e-6));
    const float length = std::sqrt(scene.rotation[0] * scene.rotation[0] +
                                   scene.rotation[1] * scene.rotation[1] +
                                   scene.rotation[2] * scene.rotation[2] +
                                   scene.rotation[3] * scene.rotation[3]);
    CHECK(length == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("spz v1 stores fp16 positions, not 24-bit fixed point") {
    // (0, 0, 1) as three fp16 values; the v3 body would have 9 bytes here and
    // shift every later stream by 3 bytes.
    const std::vector<uint8_t> bytes =
        gzip(spz_v1_body(0.0f, 0.0f, 1.0f, 128, 128, 160, 128, 128, 128));

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_spz(bytes.data(), bytes.size(), limits, &report);
    REQUIRE_MESSAGE(imported.ok(), imported.message());
    CHECK(report.note == "spz v1");
    const SplatScene& scene = imported.value();
    REQUIRE(scene.count() == 1);
    CHECK(scene.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(scene.position[1] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(scene.position[2] == doctest::Approx(-1.0f).epsilon(1e-6));
    CHECK(scene.sh_dc[0] == doctest::Approx((128.0f / 255.0f - 0.5f) / 0.15f).epsilon(1e-5));
    CHECK(scene.log_scale[0] == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(scene.opacity_logit[0] == doctest::Approx(std::log(128.0f / 127.0f)).epsilon(1e-5));
    // The v1/v2 three-byte rotation is xyz = (byte/127.5 - 1), w from the norm.
    CHECK(scene.rotation[0] == doctest::Approx(1.0f).epsilon(2e-2));
    CHECK(std::abs(scene.rotation[1]) < 2e-2f);
    CHECK(std::abs(scene.rotation[2]) < 2e-2f);
    CHECK(std::abs(scene.rotation[3]) < 2e-2f);
}

TEST_CASE(".compressed.ply dequantises positions, scales, colours and rotations") {
    SUBCASE("18-property chunk honours the colour range") {
        const std::vector<uint8_t> bytes = compressed_ply(true);
        ImportLimits limits;
        ImportReport report;
        const splat::Result<SplatScene> imported = splat::import_compressed_ply(
            bytes.data(), bytes.size(), limits, &report);
        REQUIRE_MESSAGE(imported.ok(), imported.message());
        CHECK(report.format == SplatFormat::compressed_ply);
        CHECK(report.note == "compressed_ply (colour range)");
        const SplatScene& scene = imported.value();
        REQUIRE(scene.count() == 2);

        // Splat 0: every packed field zero, so each axis sits at its minimum.
        CHECK(scene.position[0] == doctest::Approx(0.0f).epsilon(1e-6));
        CHECK(scene.position[1] == doctest::Approx(-1.0f).epsilon(1e-6));
        CHECK(scene.position[2] == doctest::Approx(10.0f).epsilon(1e-6));
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK(scene.log_scale[axis] == doctest::Approx(-6.0f).epsilon(1e-6));
        }
        // 8-bit 255 with a 0..1 range is 1.0 -> sh_dc = 0.5 / SH_C0.
        CHECK(scene.sh_dc[0] == doctest::Approx(0.5f / kSHC0).epsilon(1e-5));
        // Alpha 255 clamps to 254 -> ln(254/1).
        CHECK(scene.opacity_logit[0] == doctest::Approx(std::log(254.0f)).epsilon(1e-4));

        // Splat 1: all three fields at their maximum.
        CHECK(scene.position[3] == doctest::Approx(2.0f).epsilon(1e-6));
        CHECK(scene.position[4] == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(scene.position[5] == doctest::Approx(12.0f).epsilon(1e-6));
        for (size_t axis = 0; axis < 3; ++axis) {
            CHECK(scene.log_scale[3 + axis] == doctest::Approx(-2.0f).epsilon(1e-6));
        }
        // 128/255 = 0.50196 with a 0..1 range.
        CHECK(scene.sh_dc[3] == doctest::Approx((128.0f / 255.0f - 0.5f) / kSHC0).epsilon(1e-5));
        CHECK(scene.opacity_logit[1] == doctest::Approx(std::log(128.0f / 127.0f)).epsilon(1e-5));
        // The identity rotation: largest component w = 1.
        CHECK(scene.rotation[4] == doctest::Approx(1.0f).epsilon(2e-3));
    }

    SUBCASE("12-property chunk uses the 8-bit colour range") {
        const std::vector<uint8_t> bytes = compressed_ply(false);
        ImportLimits limits;
        ImportReport report;
        const splat::Result<SplatScene> imported = splat::import_compressed_ply(
            bytes.data(), bytes.size(), limits, &report);
        REQUIRE_MESSAGE(imported.ok(), imported.message());
        CHECK(report.note == "compressed_ply (8-bit colour)");
        const SplatScene& scene = imported.value();
        REQUIRE(scene.count() == 2);
        // 255/255 and 128/255 with no colour range.
        CHECK(scene.sh_dc[0] == doctest::Approx(0.5f / kSHC0).epsilon(1e-5));
        CHECK(scene.sh_dc[3] == doctest::Approx((128.0f / 255.0f - 0.5f) / kSHC0).epsilon(1e-5));
        CHECK(scene.position[4] == doctest::Approx(1.0f).epsilon(1e-6));
    }
}

TEST_CASE("sniff_splat_format reads the header, not the extension") {
    const splat::Path ply_path("scene.ply");
    const splat::Path splat_path("scene.splat");

    const std::string psplat = std::string("PSPLAT\0\0", 8) + "rest";
    CHECK(splat::sniff_splat_format(splat_path, reinterpret_cast<const uint8_t*>(psplat.data()),
                                    psplat.size()) == SplatFormat::psplat);

    const std::string ply = "ply\nformat binary_little_endian 1.0\nelement vertex 0\n";
    CHECK(splat::sniff_splat_format(ply_path, reinterpret_cast<const uint8_t*>(ply.data()),
                                    ply.size()) == SplatFormat::ply);

    const std::string compressed =
        "ply\nformat binary_little_endian 1.0\nelement chunk 1\nproperty float min_x\n"
        "element vertex 1\nproperty uint packed_position\n";
    CHECK(splat::sniff_splat_format(ply_path, reinterpret_cast<const uint8_t*>(compressed.data()),
                                    compressed.size()) == SplatFormat::compressed_ply);

    const uint8_t gzip_prefix[8] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(splat::sniff_splat_format(splat_path, gzip_prefix, sizeof(gzip_prefix)) ==
          SplatFormat::spz);

    const uint8_t ngsp_prefix[8] = {'N', 'G', 'S', 'P', 4, 0, 0, 0};
    CHECK(splat::sniff_splat_format(splat_path, ngsp_prefix, sizeof(ngsp_prefix)) ==
          SplatFormat::spz);

    // .splat has no magic: the extension and a whole number of 32-byte records.
    const uint8_t records[32] = {0};
    CHECK(splat::sniff_splat_format(splat_path, records, sizeof(records)) == SplatFormat::splat);
    CHECK(splat::sniff_splat_format(splat_path, records, 31) == SplatFormat::unknown);
    CHECK(splat::sniff_splat_format(splat::Path("scene.bin"), records, sizeof(records)) ==
          SplatFormat::unknown);

    CHECK(splat::is_splat_file(splat::Path("a.splat")));
    CHECK(splat::is_splat_file(splat::Path("a.compressed.ply")));
    CHECK(splat::is_splat_file(splat::Path("a.SPZ")));
    CHECK(!splat::is_splat_file(splat::Path("a.jpg")));
}

TEST_CASE("an NGSP header shorter than the version field is refused, not read past") {
    // The version lives at bytes 4-7. A 4-to-7-byte file that starts with the
    // magic is a truncated download; reading the version there used to be a
    // heap over-read. The reader must answer from the size it was handed.
    for (const std::size_t size : {std::size_t{4}, std::size_t{5}, std::size_t{6},
                                   std::size_t{7}}) {
        std::vector<uint8_t> bytes = {'N', 'G', 'S', 'P'};
        bytes.resize(size, 0xFF);
        ImportLimits limits;
        ImportReport report;
        const splat::Result<SplatScene> scene =
            splat::import_spz(bytes.data(), bytes.size(), limits, &report);
        REQUIRE_MESSAGE(!scene.ok(), "size " << size << " was accepted");
        CHECK(scene.message().find("spz header is truncated") != std::string::npos);
    }
}

TEST_CASE("a header declaring too many splats is refused before allocation") {
    // The header alone: 21,875,000 vertices is "700 MB of 32-byte .splat
    // records", above the 8,000,000-source guard. The reader must answer from
    // the header, so the buffer holds no vertex data at all.
    std::vector<uint8_t> bytes;
    put_text(bytes, ply_header(1, 21875000, false));
    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_compressed_ply(bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("21,875,000") != std::string::npos);
    CHECK(imported.message().find("8,000,000") != std::string::npos);
    CHECK(imported.message().find("limit") != std::string::npos);
}

TEST_CASE("read_scene_path imports a .spz fixture with its own count") {
    splat_test::TempDir temp("splat-path");
    const splat::Path path = temp.path() / "robot.spz";
    const std::vector<uint8_t> bytes =
        gzip(spz_v3_body(0, 0, 1 << 12, 128, 128, 160, kSpzIdentityRotation));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();

    const splat::Result<splat::LoadedScene> loaded = splat::read_scene_path(path);
    REQUIRE_MESSAGE(loaded.ok(), loaded.message());
    CHECK(loaded.value().scene.count() == 1);
    CHECK(loaded.value().meta.backend == "import");
    CHECK(loaded.value().meta.origin_format == "spz");
    CHECK(loaded.value().meta.origin_count == 1);
    CHECK(loaded.value().meta.splat_count == 1);
}

TEST_CASE("a chunk count that would overflow the size check is refused") {
    // 2^60 chunks of 48 bytes each: the multiplication wraps to 0 in a reader
    // that multiplies before comparing, so the truncation check must divide.
    std::vector<uint8_t> bytes;
    put_text(bytes, ply_header(size_t{1} << 60, 1, false));
    for (int i = 0; i < 16; ++i) bytes.push_back(0);  // one 4-uint vertex record

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_compressed_ply(bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("truncated") != std::string::npos);
}

TEST_CASE("a partial colour range is refused instead of read before the chunk") {
    // min_r and max_r only: the 12/18 split must be all six or none, because a
    // missing min_g would index one byte before the chunk record.
    std::string header = "ply\nformat binary_little_endian 1.0\n";
    header += "element chunk 1\n";
    for (const char* name : {"min_x", "min_y", "min_z", "max_x", "max_y", "max_z",
                             "min_scale_x", "min_scale_y", "min_scale_z",
                             "max_scale_x", "max_scale_y", "max_scale_z",
                             "min_r", "max_r"}) {
        header += std::string("property float ") + name + "\n";
    }
    header += "element vertex 1\nproperty uint packed_position\n"
              "property uint packed_rotation\nproperty uint packed_scale\n"
              "property uint packed_color\nend_header\n";
    std::vector<uint8_t> bytes;
    put_text(bytes, header);
    for (int i = 0; i < 14; ++i) put_f32(bytes, 0.0f);
    for (int i = 0; i < 4; ++i) put_u32(bytes, 0u);

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_compressed_ply(bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("colour range") != std::string::npos);
}

TEST_CASE("a non-finite chunk range is refused before it reaches thinning") {
    std::vector<uint8_t> bytes = compressed_ply(true);
    const size_t chunk = ply_header(1, 2, true).size();
    // min_scale_x is the seventh float of the chunk record.
    const uint32_t nan_bits = 0x7FC00000u;
    bytes[chunk + 24 + 0] = static_cast<uint8_t>(nan_bits & 0xFFu);
    bytes[chunk + 24 + 1] = static_cast<uint8_t>((nan_bits >> 8) & 0xFFu);
    bytes[chunk + 24 + 2] = static_cast<uint8_t>((nan_bits >> 16) & 0xFFu);
    bytes[chunk + 24 + 3] = static_cast<uint8_t>((nan_bits >> 24) & 0xFFu);

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_compressed_ply(bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("non-finite") != std::string::npos);
    CHECK(imported.message().find("min_scale_x") != std::string::npos);
}

TEST_CASE("an spz v4 stream size of 2^64-1 is refused, not decompressed") {
    // One point, five streams, but the first TOC entry claims SIZE_MAX bytes.
    std::vector<uint8_t> header;
    put_u32(header, 0x5053474eu);  // NGSP
    put_u32(header, 4);            // version
    put_u32(header, 1);            // numPoints
    header.push_back(0);           // shDegree
    header.push_back(12);          // fractionalBits
    header.push_back(0);           // flags
    header.push_back(5);           // numStreams
    put_u32(header, 32);           // tocByteOffset: right after the header
    for (int i = 0; i < 12; ++i) header.push_back(0);
    REQUIRE(header.size() == 32);

    std::vector<uint8_t> bytes = header;
    put_u64(bytes, 0xFFFFFFFFFFFFFFFFull);  // compressedSize
    put_u64(bytes, 9);                      // uncompressedSize, the position stream
    for (int i = 1; i < 5; ++i) {
        put_u64(bytes, 0);
        put_u64(bytes, 0);
    }

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported =
        splat::import_spz(bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("truncated") != std::string::npos);
}

TEST_CASE("a .psplat count above the source limit is refused before allocation") {
    // An 80-byte header that declares 10,000,000 splats: the fp16 payload would
    // be 560 MB, under the file guard, so the count guard has to fire first.
    std::vector<uint8_t> bytes;
    put_text(bytes, std::string("PSPLAT\0\0", 8));
    put_u32(bytes, 1);          // version
    put_u32(bytes, 10000000);   // count
    put_u32(bytes, 14);         // values per splat
    put_u32(bytes, 1);          // layout
    while (bytes.size() < splat::kPsplatHeaderSize) bytes.push_back(0);

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported = splat::import_splat(
        splat::Path("huge.psplat"), bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("10,000,000") != std::string::npos);
    CHECK(imported.message().find("8,000,000") != std::string::npos);
    CHECK(imported.message().find("limit") != std::string::npos);
}

TEST_CASE("a .ply vertex count above the source limit is refused before reading") {
    splat_test::TempDir temp("ply-guard");
    const splat::Path path = temp.path() / "huge.ply";
    std::string header = "ply\nformat binary_little_endian 1.0\nelement vertex 10000000\n";
    for (const char* name : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                             "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2",
                             "rot_3"}) {
        header += std::string("property float ") + name + "\n";
    }
    header += "end_header\n";
    std::vector<uint8_t> bytes;
    put_text(bytes, header);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();

    ImportLimits limits;
    ImportReport report;
    const splat::Result<SplatScene> imported = splat::import_splat(
        path, bytes.data(), bytes.size(), limits, &report);
    REQUIRE(!imported.ok());
    CHECK(imported.message().find("10000000") != std::string::npos);
    CHECK(imported.message().find("8000000") != std::string::npos);
    CHECK(imported.message().find("limit") != std::string::npos);
}

TEST_CASE("real exporter fixtures import when they are present") {
    // tests/fixtures/splats/README.md: the files are not in the repository, so
    // each check skips when the file is absent.
    const splat::Path dir = splat_test::repo_root() / "tests" / "fixtures" / "splats";
    struct Fixture {
        const char* name;
        SplatFormat format;
    };
    const Fixture fixtures[] = {
        {"spz_v4.spz", SplatFormat::spz},
        {"supersplat.compressed.ply", SplatFormat::compressed_ply},
        {"antimatter15.splat", SplatFormat::splat},
    };
    for (const Fixture& fixture : fixtures) {
        const splat::Path path = dir / fixture.name;
        if (!std::filesystem::exists(path)) {
            MESSAGE("skipped " << fixture.name << ": not in tests/fixtures/splats");
            continue;
        }
        const splat::Result<std::vector<uint8_t>> bytes =
            splat::read_file_bytes(path, splat::kMaxSplatFileBytes);
        REQUIRE_MESSAGE(bytes.ok(), fixture.name << ": " << bytes.message());
        ImportLimits limits;
        ImportReport report;
        const splat::Result<SplatScene> imported = splat::import_splat(
            path, bytes.value().data(), bytes.value().size(), limits, &report);
        REQUIRE_MESSAGE(imported.ok(), fixture.name << ": " << imported.message());
        CHECK(report.format == fixture.format);
        CHECK(imported.value().count() > 0);
        CHECK(imported.value().consistent());
    }
}
