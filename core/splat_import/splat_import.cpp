// SPDX-License-Identifier: GPL-3.0-or-later

#include "splat_import/splat_import.h"

#include <algorithm>
#include <string>

#include "scene/byte_order.h"
#include "scene/ply.h"
#include "scene/psplat.h"

namespace splat {
namespace {

std::string bytes_label(uint64_t bytes) {
    if (bytes % (1024ull * 1024) == 0) return std::to_string(bytes / (1024ull * 1024)) + " MiB";
    return std::to_string(bytes) + " bytes";
}

}  // namespace

std::string count_label(uint64_t count) {
    const std::string digits = std::to_string(count);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (size_t index = 0; index < digits.size(); ++index) {
        if (index >= lead && (index - lead) % 3 == 0) out.push_back(',');
        out.push_back(digits[index]);
    }
    return out;
}

Status check_source_count(uint64_t declared, const ImportLimits& limits) {
    if (declared > limits.max_source_splats) {
        return Status::error("declared " + count_label(declared) + " splats, above the " +
                             count_label(limits.max_source_splats) + "-splat import limit");
    }
    return Status::success();
}

Result<SplatScene> import_splat(const Path& path, const uint8_t* data, size_t size,
                                const ImportLimits& limits, ImportReport* report) {
    ImportReport local;
    ImportReport* out = report != nullptr ? report : &local;
    *out = ImportReport{};

    if (data == nullptr || size == 0) {
        return Result<SplatScene>::failure("splat file is empty: " + path.string());
    }
    // File-size guard before anything is parsed or allocated.
    if (size > limits.max_bytes) {
        return Result<SplatScene>::failure(
            "splat file is " + bytes_label(size) + ", larger than the " +
            bytes_label(limits.max_bytes) + " import limit");
    }

    const SplatFormat format = sniff_splat_format(path, data, size);
    out->format = format;

    Result<SplatScene> scene = Result<SplatScene>::failure("unsupported splat format");
    switch (format) {
        case SplatFormat::psplat: {
            // The count is a fixed header field; guard it here so read_psplat
            // never sizes a scene above the source limit.
            if (size >= kPsplatHeaderSize) {
                const Status allowed = check_source_count(load_u32(data + 12), limits);
                if (!allowed.ok()) return Result<SplatScene>::failure(allowed.message());
            }
            const Result<SplatScene> loaded = read_psplat(data, size);
            if (!loaded.ok()) return loaded;
            scene = Result<SplatScene>::success(loaded.value());
            break;
        }
        case SplatFormat::ply: {
            // The existing 3DGS reader streams the file itself; there is no
            // second PLY parser here on purpose. The caller passes the bytes so
            // the format could be sniffed, and gives the path for this reader.
            // The count limit goes in because that reader allocates the vertex
            // block before import_splat could see the scene.
            if (path.empty()) {
                return Result<SplatScene>::failure("a .ply import needs a file path");
            }
            SplatScene loaded;
            const Status status = import_ply(path, loaded, limits.max_source_splats);
            if (!status.ok()) return Result<SplatScene>::failure(status.message());
            scene = Result<SplatScene>::success(std::move(loaded));
            break;
        }
        case SplatFormat::splat: scene = import_splat_records(data, size, limits, out); break;
        case SplatFormat::spz: scene = import_spz(data, size, limits, out); break;
        case SplatFormat::compressed_ply:
            scene = import_compressed_ply(data, size, limits, out);
            break;
        case SplatFormat::unknown:
            return Result<SplatScene>::failure(
                "unsupported splat file (no known header): " + path.string());
    }
    if (!scene.ok()) return scene;

    if (!scene.value().consistent()) {
        return Result<SplatScene>::failure("import produced an inconsistent scene");
    }
    out->source_count = static_cast<uint32_t>(scene.value().count());
    out->kept_count = out->source_count;
    if (out->note.empty()) out->note = splat_format_name(format);
    return scene;
}

}  // namespace splat
