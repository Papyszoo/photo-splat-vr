#include "scene/ply.h"

#include "scene/byte_order.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace splat {
namespace {

struct PlyProperty {
    std::string name;
    std::string type;
    size_t offset = 0;
};

struct PlyElement {
    std::string name;
    size_t count = 0;
    size_t stride = 0;
    std::vector<PlyProperty> properties;

    const PlyProperty* find(const std::string& property) const {
        for (const PlyProperty& candidate : properties) {
            if (candidate.name == property) return &candidate;
        }
        return nullptr;
    }
};

// Elements in file order, so the payload offset of every element is the sum of
// the preceding strides.
struct PlyHeader {
    std::streamoff data_offset = 0;
    std::vector<PlyElement> elements;

    const PlyElement* find(const std::string& element) const {
        for (const PlyElement& candidate : elements) {
            if (candidate.name == element) return &candidate;
        }
        return nullptr;
    }
};

bool type_size(const std::string& type, size_t& size) {
    if (type == "char" || type == "int8" || type == "uchar" || type == "uint8") {
        size = 1;
        return true;
    }
    if (type == "short" || type == "int16" || type == "ushort" || type == "uint16") {
        size = 2;
        return true;
    }
    if (type == "int" || type == "int32" || type == "uint" || type == "uint32" ||
        type == "float" || type == "float32") {
        size = 4;
        return true;
    }
    if (type == "double" || type == "float64") {
        size = 8;
        return true;
    }
    return false;
}

bool is_float_type(const std::string& type) { return type == "float" || type == "float32"; }

std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start < line.size()) {
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        size_t end = start;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
        if (end > start) words.push_back(line.substr(start, end - start));
        start = end;
    }
    return words;
}

Status read_ply_header(std::ifstream& file, PlyHeader& header) {
    std::string line;
    if (!std::getline(file, line)) return Status::error("empty .ply");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "ply") return Status::error("not a .ply (missing magic)");

    bool have_format = false;
    bool header_done = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> words = split_words(line);
        if (words.empty()) continue;

        if (words[0] == "format") {
            if (words.size() < 2 || words[1] != "binary_little_endian") {
                return Status::error("only binary_little_endian .ply is supported");
            }
            have_format = true;
        } else if (words[0] == "element") {
            if (words.size() < 3) return Status::error("malformed element line in .ply header");
            PlyElement element;
            element.name = words[1];
            try {
                element.count = static_cast<size_t>(std::stoull(words[2]));
            } catch (const std::exception&) {
                return Status::error("invalid element count in .ply header: " + words[2]);
            }
            header.elements.push_back(std::move(element));
        } else if (words[0] == "property") {
            if (words.size() < 3) return Status::error("malformed property line in .ply header");
            if (header.elements.empty()) return Status::error("property line before any element");
            size_t size = 0;
            const std::string& type = words[1];
            const std::string& name = words.back();
            if (!type_size(type, size)) {
                return Status::error("unsupported .ply property type " + type);
            }
            PlyElement& element = header.elements.back();
            element.properties.push_back(PlyProperty{name, type, element.stride});
            element.stride += size;
        } else if (words[0] == "end_header") {
            header_done = true;
            break;
        }
    }

    if (!header_done) return Status::error(".ply header has no end_header");
    if (!have_format) return Status::error(".ply header has no format line");
    if (header.elements.empty()) return Status::error(".ply has no elements");

    const std::streamoff data_offset = file.tellg();
    if (data_offset < 0) return Status::error("cannot determine .ply data offset");
    header.data_offset = data_offset;
    return Status::success();
}

}  // namespace

Status import_ply(const Path& path, SplatScene& scene) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Status::error("cannot open " + path.string());

    PlyHeader header;
    const Status parsed = read_ply_header(file, header);
    if (!parsed.ok()) return parsed;

    const PlyElement* vertex = header.find("vertex");
    if (vertex == nullptr) return Status::error(".ply has no vertex element");
    if (header.elements.empty() || header.elements.front().name != "vertex") {
        return Status::error(".ply vertex element must come first");
    }

    static const std::array<const char*, 14> kRequired{{
        "x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
        "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}};
    for (const char* name : kRequired) {
        const PlyProperty* property = vertex->find(name);
        if (property == nullptr) return Status::error(std::string(".ply is missing vertex property ") + name);
        if (!is_float_type(property->type)) {
            return Status::error(std::string(".ply vertex property ") + name + " is not float32");
        }
    }

    if (vertex->count > 0xffffffffu) return Status::error(".ply vertex count exceeds uint32");
    if (vertex->stride != 0 && vertex->count > (size_t(-1) / vertex->stride)) {
        return Status::error(".ply vertex data size overflows");
    }
    const size_t data_bytes = vertex->count * vertex->stride;

    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    if (file_size < header.data_offset ||
        static_cast<size_t>(file_size - header.data_offset) < data_bytes) {
        return Status::error(".ply vertex data is truncated");
    }
    file.seekg(header.data_offset, std::ios::beg);
    std::vector<uint8_t> data(data_bytes);
    if (data_bytes > 0) {
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data_bytes));
        if (!file) return Status::error("failed while reading .ply vertex data");
    }

    scene.resize(vertex->count);
    const PlyProperty* x = vertex->find("x");
    const PlyProperty* y = vertex->find("y");
    const PlyProperty* z = vertex->find("z");
    const PlyProperty* fdc0 = vertex->find("f_dc_0");
    const PlyProperty* fdc1 = vertex->find("f_dc_1");
    const PlyProperty* fdc2 = vertex->find("f_dc_2");
    const PlyProperty* opacity = vertex->find("opacity");
    const PlyProperty* scale0 = vertex->find("scale_0");
    const PlyProperty* scale1 = vertex->find("scale_1");
    const PlyProperty* scale2 = vertex->find("scale_2");
    const PlyProperty* rot0 = vertex->find("rot_0");
    const PlyProperty* rot1 = vertex->find("rot_1");
    const PlyProperty* rot2 = vertex->find("rot_2");
    const PlyProperty* rot3 = vertex->find("rot_3");

    for (size_t i = 0; i < vertex->count; ++i) {
        const uint8_t* row = data.data() + i * vertex->stride;
        scene.position[i * 3 + 0] = load_f32(row + x->offset);
        scene.position[i * 3 + 1] = load_f32(row + y->offset);
        scene.position[i * 3 + 2] = load_f32(row + z->offset);
        scene.sh_dc[i * 3 + 0] = load_f32(row + fdc0->offset);
        scene.sh_dc[i * 3 + 1] = load_f32(row + fdc1->offset);
        scene.sh_dc[i * 3 + 2] = load_f32(row + fdc2->offset);
        scene.opacity_logit[i] = load_f32(row + opacity->offset);
        scene.log_scale[i * 3 + 0] = load_f32(row + scale0->offset);
        scene.log_scale[i * 3 + 1] = load_f32(row + scale1->offset);
        scene.log_scale[i * 3 + 2] = load_f32(row + scale2->offset);
        scene.rotation[i * 4 + 0] = load_f32(row + rot0->offset);
        scene.rotation[i * 4 + 1] = load_f32(row + rot1->offset);
        scene.rotation[i * 4 + 2] = load_f32(row + rot2->offset);
        scene.rotation[i * 4 + 3] = load_f32(row + rot3->offset);
    }

    return Status::success();
}

Status export_ply(const Path& path, const SplatScene& scene) {
    if (!scene.consistent()) return Status::error(".ply scene arrays have inconsistent sizes");

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error("cannot open " + path.string() + " for writing");

    out << "ply\n"
        << "format binary_little_endian 1.0\n"
        << "comment written by ps5-splat-vr\n"
        << "element vertex " << scene.count() << "\n"
        << "property float x\n"
        << "property float y\n"
        << "property float z\n"
        << "property float f_dc_0\n"
        << "property float f_dc_1\n"
        << "property float f_dc_2\n"
        << "property float opacity\n"
        << "property float scale_0\n"
        << "property float scale_1\n"
        << "property float scale_2\n"
        << "property float rot_0\n"
        << "property float rot_1\n"
        << "property float rot_2\n"
        << "property float rot_3\n"
        << "end_header\n";

    std::array<uint8_t, 4096> buffer{};
    size_t used = 0;
    auto flush = [&]() {
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(used));
        used = 0;
    };
    auto put = [&](float value) {
        if (used + 4 > buffer.size()) flush();
        store_f32(buffer.data() + used, value);
        used += 4;
    };

    for (size_t i = 0; i < scene.count(); ++i) {
        put(scene.position[i * 3 + 0]);
        put(scene.position[i * 3 + 1]);
        put(scene.position[i * 3 + 2]);
        put(scene.sh_dc[i * 3 + 0]);
        put(scene.sh_dc[i * 3 + 1]);
        put(scene.sh_dc[i * 3 + 2]);
        put(scene.opacity_logit[i]);
        put(scene.log_scale[i * 3 + 0]);
        put(scene.log_scale[i * 3 + 1]);
        put(scene.log_scale[i * 3 + 2]);
        put(scene.rotation[i * 4 + 0]);
        put(scene.rotation[i * 4 + 1]);
        put(scene.rotation[i * 4 + 2]);
        put(scene.rotation[i * 4 + 3]);
    }
    if (used > 0) flush();

    if (!out) return Status::error("failed while writing " + path.string());
    return Status::success();
}

Result<Intrinsics> read_ply_intrinsics(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<Intrinsics>::failure("cannot open " + path.string());

    PlyHeader header;
    const Status parsed = read_ply_header(file, header);
    if (!parsed.ok()) return Result<Intrinsics>::failure(parsed.message());

    Intrinsics out;
    std::streamoff offset = header.data_offset;
    for (const PlyElement& element : header.elements) {
        const bool is_float = element.properties.size() == 1 &&
                              is_float_type(element.properties.front().type);
        // `element intrinsic 9` + `property float intrinsic` is nine floats,
        // one per item, so the stride is a single float and the count is 9.
        if (element.name == "intrinsic" && is_float && element.count == 9 &&
            element.stride == sizeof(float)) {
            float values[9] = {};
            file.seekg(offset, std::ios::beg);
            if (file.read(reinterpret_cast<char*>(values), sizeof(values))) {
                out.fx = values[0];
                out.cx = values[2];
                out.fy = values[4];
                out.cy = values[5];
            }
        } else if (element.name == "image_size" && element.count == 2 &&
                   element.stride == sizeof(uint32_t) && element.properties.size() == 1 &&
                   element.properties.front().type == "uint") {
            uint32_t size[2] = {};
            file.seekg(offset, std::ios::beg);
            if (file.read(reinterpret_cast<char*>(size), sizeof(size))) {
                out.width = size[0];
                out.height = size[1];
            }
        }
        if (element.count != 0 && element.stride > (size_t(-1) / element.count)) {
            return Result<Intrinsics>::failure(".ply element sizes overflow");
        }
        offset += static_cast<std::streamoff>(element.count * element.stride);
    }

    // A .ply without SHARP's extra elements (or with a nonsensical one) simply
    // has no usable intrinsics; the caller falls back to a default field of view.
    if (!(out.fx > 0.0f) || !(out.fy > 0.0f) || out.width == 0 || out.height == 0) {
        return Result<Intrinsics>::success(Intrinsics{});
    }
    return Result<Intrinsics>::success(out);
}

}  // namespace splat
