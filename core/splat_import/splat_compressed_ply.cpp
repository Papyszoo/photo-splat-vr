// SuperSplat / PlayCanvas .compressed.ply (SPEC Appendix A.3): a binary
// little-endian PLY with a quantised chunk element plus four packed uint32
// words per splat.
//
// This is not a second 3DGS PLY reader: the vertex layout, the property names
// and the meaning of every word are different, and the existing import_ply()
// would refuse the file. The header is parsed only far enough to find the two
// elements and their property order, so a file that also carries an SH element
// (which we drop) reads the same.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "scene/byte_order.h"
#include "splat_import/smallest_three.h"
#include "splat_import/splat_import.h"

namespace splat {
namespace {

constexpr float kSHC0 = 0.28209479177387814f;
constexpr size_t kSplatsPerChunk = 256;
// A header longer than this is not a PLY from any exporter; the search for
// end_header stops there instead of walking a binary payload.
constexpr size_t kMaxHeaderBytes = 64 * 1024;

struct PlyProperty {
    std::string type;
    std::string name;
    bool list = false;
};

struct PlyElement {
    std::string name;
    size_t count = 0;
    std::vector<PlyProperty> properties;
};

struct PlyHeader {
    std::vector<PlyElement> elements;
    size_t data_offset = 0;
};

size_t type_size(const std::string& type) {
    if (type == "char" || type == "uchar" || type == "int8" || type == "uint8") return 1;
    if (type == "short" || type == "ushort" || type == "int16" || type == "uint16") return 2;
    if (type == "int" || type == "uint" || type == "int32" || type == "uint32" ||
        type == "float" || type == "float32") {
        return 4;
    }
    if (type == "double" || type == "float64") return 8;
    return 0;
}

// The header as text, up to and including the end_header line. Everything
// after it is the binary payload whose offset is what the caller needs.
Result<PlyHeader> parse_header(const uint8_t* data, size_t size) {
    const size_t window = std::min(size, kMaxHeaderBytes);
    const std::string text(reinterpret_cast<const char*>(data), window);
    if (text.compare(0, 3, "ply") != 0) return Result<PlyHeader>::failure("not a PLY header");

    const size_t marker = text.find("end_header");
    if (marker == std::string::npos) {
        return Result<PlyHeader>::failure("PLY header has no end_header line");
    }
    size_t offset = text.find('\n', marker);
    if (offset == std::string::npos) {
        return Result<PlyHeader>::failure("PLY header is not newline terminated");
    }
    ++offset;  // the payload starts on the next byte

    PlyHeader header;
    header.data_offset = offset;

    std::istringstream lines(text.substr(0, offset));
    std::string line;
    std::getline(lines, line);
    if (line.compare(0, 3, "ply") != 0) return Result<PlyHeader>::failure("not a PLY header");

    bool binary_little = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream tokens(line);
        std::string keyword;
        tokens >> keyword;
        if (keyword == "format") {
            std::string format;
            tokens >> format;
            if (format != "binary_little_endian") {
                return Result<PlyHeader>::failure(
                    "compressed PLY must be binary_little_endian, not '" + format + "'");
            }
            binary_little = true;
        } else if (keyword == "element") {
            PlyElement element;
            tokens >> element.name >> element.count;
            if (element.name.empty()) return Result<PlyHeader>::failure("PLY element has no name");
            header.elements.push_back(std::move(element));
        } else if (keyword == "property") {
            if (header.elements.empty()) {
                return Result<PlyHeader>::failure("PLY property outside an element");
            }
            PlyProperty property;
            tokens >> property.type;
            if (property.type == "list") {
                property.list = true;
                std::string count_type;
                std::string item_type;
                tokens >> count_type >> item_type >> property.name;
                property.type = item_type;
            } else {
                tokens >> property.name;
            }
            if (property.name.empty() || type_size(property.type) == 0) {
                return Result<PlyHeader>::failure("PLY property '" + property.name +
                                                  "' has an unknown type");
            }
            header.elements.back().properties.push_back(std::move(property));
        }
    }
    if (!binary_little) {
        return Result<PlyHeader>::failure("PLY header has no binary_little_endian format line");
    }
    return Result<PlyHeader>::success(std::move(header));
}

const PlyElement* find_element(const PlyHeader& header, const std::string& name) {
    for (const PlyElement& element : header.elements) {
        if (element.name == name) return &element;
    }
    return nullptr;
}

// Byte offset of `name` inside one record of `element`, or -1 when the element
// does not carry it.
int property_offset(const PlyElement& element, const std::string& name) {
    int offset = 0;
    for (const PlyProperty& property : element.properties) {
        if (property.list) return -1;  // a variable-size record cannot be indexed
        if (property.name == name) return offset;
        offset += static_cast<int>(type_size(property.type));
    }
    return -1;
}

size_t record_size(const PlyElement& element) {
    size_t size = 0;
    for (const PlyProperty& property : element.properties) size += type_size(property.type);
    return size;
}

// 11 bits for x, 10 for y, 11 for z, packed from the top of the word.
float unquantise(uint32_t value, uint32_t bits, float low, float high) {
    const uint32_t maximum = (1u << bits) - 1u;
    return low + (static_cast<float>(value) / static_cast<float>(maximum)) * (high - low);
}

}  // namespace

Result<SplatScene> import_compressed_ply(const uint8_t* data, size_t size,
                                         const ImportLimits& limits, ImportReport* report) {
    ImportReport local;
    ImportReport* out = report != nullptr ? report : &local;
    out->format = SplatFormat::compressed_ply;

    if (data == nullptr || size == 0) {
        return Result<SplatScene>::failure("compressed PLY is empty");
    }
    const Result<PlyHeader> parsed = parse_header(data, size);
    if (!parsed.ok()) return Result<SplatScene>::failure(parsed.message());
    const PlyHeader& header = parsed.value();

    const PlyElement* chunks = find_element(header, "chunk");
    const PlyElement* vertices = find_element(header, "vertex");
    if (chunks == nullptr || vertices == nullptr) {
        return Result<SplatScene>::failure(
            "compressed PLY needs a chunk and a vertex element");
    }
    if (chunks->count == 0 && vertices->count != 0) {
        return Result<SplatScene>::failure("compressed PLY has vertices but no chunks");
    }

    // The chunk holds the dequantisation ranges; a file without them cannot be
    // decoded, so say which name is missing instead of producing zeros.
    const char* const chunk_names[12] = {"min_x",  "min_y",  "min_z",       "max_x",
                                         "max_y",  "max_z",  "min_scale_x", "min_scale_y",
                                         "min_scale_z", "max_scale_x", "max_scale_y",
                                         "max_scale_z"};
    int chunk_offset[12];
    for (int i = 0; i < 12; ++i) {
        chunk_offset[i] = property_offset(*chunks, chunk_names[i]);
        if (chunk_offset[i] < 0) {
            return Result<SplatScene>::failure(std::string("compressed PLY chunk has no ") +
                                               chunk_names[i]);
        }
    }
    // The colour range is the 18-property variant; a 12-property chunk stores
    // 0..255 bytes instead. A chunk with only some of the six names is neither,
    // and reading a missing one used to index before the record, so the split is
    // all six or none.
    const char* const colour_names[6] = {"min_r", "min_g", "min_b",
                                         "max_r", "max_g", "max_b"};
    int colour_offset[6];
    int colour_present = 0;
    for (int i = 0; i < 6; ++i) {
        colour_offset[i] = property_offset(*chunks, colour_names[i]);
        if (colour_offset[i] >= 0) ++colour_present;
    }
    if (colour_present != 0 && colour_present != 6) {
        return Result<SplatScene>::failure(
            "compressed PLY chunk has only " + std::to_string(colour_present) +
            " of the six colour range properties (min_r min_g min_b max_r max_g max_b)");
    }
    const bool has_colour_range = colour_present == 6;

    const int packed_position = property_offset(*vertices, "packed_position");
    const int packed_rotation = property_offset(*vertices, "packed_rotation");
    const int packed_scale = property_offset(*vertices, "packed_scale");
    const int packed_colour = property_offset(*vertices, "packed_color");
    if (packed_position < 0 || packed_rotation < 0 || packed_scale < 0 || packed_colour < 0) {
        return Result<SplatScene>::failure(
            "compressed PLY vertex needs packed_position, packed_rotation, packed_scale and "
            "packed_color");
    }

    const uint64_t declared = vertices->count;
    const Status allowed = check_source_count(declared, limits);
    if (!allowed.ok()) return Result<SplatScene>::failure(allowed.message());

    const size_t chunk_stride = record_size(*chunks);
    const size_t vertex_stride = record_size(*vertices);
    const size_t available = size - header.data_offset;
    // One chunk per 256 splats. Dividing rather than multiplying keeps a corrupt
    // chunk count from wrapping the size comparison: 2^60 chunks must fail here,
    // not multiply to 0 bytes and pass a truncation check.
    const size_t needed_chunks =
        (static_cast<size_t>(declared) + kSplatsPerChunk - 1) / kSplatsPerChunk;
    if (chunks->count < needed_chunks) {
        return Result<SplatScene>::failure("compressed PLY has too few chunks for its vertices");
    }
    if (chunk_stride == 0 || chunks->count > available / chunk_stride) {
        return Result<SplatScene>::failure(
            "compressed PLY is truncated: " + std::to_string(chunks->count) +
            " chunk records do not fit in " + std::to_string(available) + " bytes of payload");
    }
    const size_t chunk_bytes = chunks->count * chunk_stride;
    const size_t vertex_bytes = static_cast<size_t>(declared) * vertex_stride;
    if (vertex_bytes > available - chunk_bytes) {
        return Result<SplatScene>::failure(
            "compressed PLY is truncated: " +
            std::to_string(chunk_bytes + vertex_bytes) + " bytes of records declared, " +
            std::to_string(available) + " in the file");
    }

    const uint8_t* const chunk_data = data + header.data_offset;
    const uint8_t* const vertex_data = chunk_data + chunk_bytes;

    // A NaN or infinite range would reach the thinning score and the renderer's
    // bounds as a NaN position/scale, so the file is refused here where the
    // offending chunk can be named.
    for (size_t c = 0; c < needed_chunks; ++c) {
        const uint8_t* const chunk = chunk_data + c * chunk_stride;
        for (int i = 0; i < 12; ++i) {
            if (!std::isfinite(load_f32(chunk + chunk_offset[i]))) {
                return Result<SplatScene>::failure(
                    "compressed PLY chunk " + std::to_string(c) + " has a non-finite " +
                    chunk_names[i] + " range value");
            }
        }
        if (has_colour_range) {
            for (int i = 0; i < 6; ++i) {
                if (!std::isfinite(load_f32(chunk + colour_offset[i]))) {
                    return Result<SplatScene>::failure(
                        "compressed PLY chunk " + std::to_string(c) + " has a non-finite " +
                        colour_names[i] + " range value");
                }
            }
        }
    }

    SplatScene scene;
    scene.resize(static_cast<size_t>(declared));
    for (size_t i = 0; i < declared; ++i) {
        const uint8_t* const chunk = chunk_data + (i / kSplatsPerChunk) * chunk_stride;
        const uint8_t* const vertex = vertex_data + i * vertex_stride;

        const uint32_t position_bits = load_u32(vertex + packed_position);
        const float low_x = load_f32(chunk + chunk_offset[0]);
        const float low_y = load_f32(chunk + chunk_offset[1]);
        const float low_z = load_f32(chunk + chunk_offset[2]);
        const float high_x = load_f32(chunk + chunk_offset[3]);
        const float high_y = load_f32(chunk + chunk_offset[4]);
        const float high_z = load_f32(chunk + chunk_offset[5]);
        scene.position[i * 3 + 0] = unquantise((position_bits >> 21) & 0x7FFu, 11, low_x, high_x);
        scene.position[i * 3 + 1] = unquantise((position_bits >> 11) & 0x3FFu, 10, low_y, high_y);
        scene.position[i * 3 + 2] = unquantise(position_bits & 0x7FFu, 11, low_z, high_z);

        const uint32_t scale_bits = load_u32(vertex + packed_scale);
        const float low_sx = load_f32(chunk + chunk_offset[6]);
        const float low_sy = load_f32(chunk + chunk_offset[7]);
        const float low_sz = load_f32(chunk + chunk_offset[8]);
        const float high_sx = load_f32(chunk + chunk_offset[9]);
        const float high_sy = load_f32(chunk + chunk_offset[10]);
        const float high_sz = load_f32(chunk + chunk_offset[11]);
        scene.log_scale[i * 3 + 0] = unquantise((scale_bits >> 21) & 0x7FFu, 11, low_sx, high_sx);
        scene.log_scale[i * 3 + 1] = unquantise((scale_bits >> 11) & 0x3FFu, 10, low_sy, high_sy);
        scene.log_scale[i * 3 + 2] = unquantise(scale_bits & 0x7FFu, 11, low_sz, high_sz);

        unpack_smallest_three_compressed_ply(load_u32(vertex + packed_rotation),
                                            &scene.rotation[i * 4]);

        const uint32_t colour_bits = load_u32(vertex + packed_colour);
        const uint8_t bytes[4] = {static_cast<uint8_t>((colour_bits >> 24) & 0xFFu),
                                  static_cast<uint8_t>((colour_bits >> 16) & 0xFFu),
                                  static_cast<uint8_t>((colour_bits >> 8) & 0xFFu),
                                  static_cast<uint8_t>(colour_bits & 0xFFu)};
        for (size_t channel = 0; channel < 3; ++channel) {
            float value = static_cast<float>(bytes[channel]) / 255.0f;
            if (has_colour_range) {
                const float low = load_f32(chunk + colour_offset[channel]);
                const float high = load_f32(chunk + colour_offset[3 + channel]);
                value = low + (value * (high - low));
            }
            scene.sh_dc[i * 3 + channel] = (value - 0.5f) / kSHC0;
        }
        const int clamped = bytes[3] < 1 ? 1 : (bytes[3] > 254 ? 254 : bytes[3]);
        const float alpha = static_cast<float>(clamped) / 255.0f;
        scene.opacity_logit[i] = std::log(alpha / (1.0f - alpha));
    }

    out->source_count = static_cast<uint32_t>(declared);
    out->kept_count = out->source_count;
    out->note = has_colour_range ? "compressed_ply (colour range)"
                                 : "compressed_ply (8-bit colour)";
    return Result<SplatScene>::success(std::move(scene));
}

}  // namespace splat
