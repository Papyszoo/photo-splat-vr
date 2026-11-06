#include "scene/scene_folder.h"

#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include "common/path.h"
#include "jobs/image_io.h"
#include "scene/ply.h"
#include "splat_import/splat_import.h"
#include "vfs/io.h"

namespace splat {
namespace {

// The file's first bytes, for a format sniff that must not read a multi-GB
// file. A shorter file yields what it has.
Result<std::vector<uint8_t>> read_prefix(const Path& path, size_t max_bytes) {
    return vfs::read_range(path, 0, max_bytes);
}

// The imported-scene marker in a parsed meta.json: the "origin" object a
// conversion never writes. One rule, used by scene_meta_from_json()'s
// origin_present and scene_meta_json_is_imported().
bool imported_marker(const nlohmann::json& root) {
    return root.is_object() && root.contains("origin") && root["origin"].is_object();
}

// A splat file read straight through the import path: the result is a scene in
// the app's own conventions, with the provenance meta.json records.
Result<LoadedScene> read_splat_file(const Path& path) {
    vfs::FileStat status;
    if (!vfs::stat(path, &status).ok() || !status.regular) {
        return Result<LoadedScene>::failure("cannot inspect " + path.string());
    }
    if (status.size > kMaxSplatFileBytes) {
        return Result<LoadedScene>::failure("splat file is larger than the 512 MiB import limit: " +
                                           path.string());
    }
    const Result<std::vector<uint8_t>> bytes =
        read_file_bytes(path, static_cast<size_t>(kMaxSplatFileBytes));
    if (!bytes.ok()) return Result<LoadedScene>::failure(bytes.message());

    ImportReport report;
    const Result<SplatScene> scene =
        import_splat(path, bytes.value().data(), bytes.value().size(), ImportLimits{}, &report);
    if (!scene.ok()) return Result<LoadedScene>::failure(scene.message());

    LoadedScene loaded;
    loaded.scene = scene.value();
    loaded.meta.backend = "import";
    loaded.meta.format_version = kPsplatVersion;
    loaded.meta.origin_format = splat_format_name(report.format);
    loaded.meta.origin_count = report.source_count;
    loaded.meta.splat_count = static_cast<uint32_t>(loaded.scene.count());
    return Result<LoadedScene>::success(std::move(loaded));
}

}  // namespace

std::string scene_meta_to_json(const SceneMeta& meta) {
    nlohmann::json root;
    root["format_version"] = meta.format_version;
    root["backend"] = meta.backend;
    root["intrinsics"] = {{"width", meta.intrinsics.width},
                          {"height", meta.intrinsics.height},
                          {"fx", meta.intrinsics.fx},
                          {"fy", meta.intrinsics.fy},
                          {"cx", meta.intrinsics.cx},
                          {"cy", meta.intrinsics.cy}};
    root["seconds_taken"] = meta.seconds_taken;
    root["splat_count"] = meta.splat_count;
    if (!meta.captured_at.empty()) root["captured_at"] = meta.captured_at;
    if (!meta.source.empty()) root["source"] = meta.source;
    if (!meta.source_folder.empty()) root["source_folder"] = meta.source_folder;
    if (!meta.origin_format.empty()) {
        root["origin"] = {{"format", meta.origin_format}, {"count", meta.origin_count}};
    }
    return root.dump(2) + "\n";
}

Result<SceneMeta> scene_meta_from_json(const std::string& json) {
    try {
        const nlohmann::json root = nlohmann::json::parse(json);
        if (!root.is_object()) return Result<SceneMeta>::failure("meta.json root is not an object");

        SceneMeta meta;
        meta.format_version = root.at("format_version").get<uint32_t>();
        meta.backend = root.at("backend").get<std::string>();
        const nlohmann::json& intrinsics = root.at("intrinsics");
        meta.intrinsics.width = intrinsics.at("width").get<uint32_t>();
        meta.intrinsics.height = intrinsics.at("height").get<uint32_t>();
        meta.intrinsics.fx = intrinsics.at("fx").get<float>();
        meta.intrinsics.fy = intrinsics.at("fy").get<float>();
        meta.intrinsics.cx = intrinsics.at("cx").get<float>();
        meta.intrinsics.cy = intrinsics.at("cy").get<float>();
        meta.seconds_taken = root.at("seconds_taken").get<double>();
        meta.splat_count = root.value("splat_count", 0u);
        meta.captured_at = root.value("captured_at", std::string{});
        meta.source = root.value("source", std::string{});
        // Stage H: absent in every meta.json written before the feature, and
        // then empty - neither a parse failure nor an "other" scene.
        meta.source_folder = root.value("source_folder", std::string{});
        // A "view" object from an older build (Track R's rotation) is ignored:
        // the viewer has no scene rotation, and a save drops the key while
        // keeping "scale" (scene/scene_view.h).
        // "origin" only exists on imported scenes; a conversion's meta.json
        // predates it and must still read. origin_present is the same marker
        // scene_is_imported() tests; the format and count are its payload.
        if (imported_marker(root)) {
            meta.origin_present = true;
            meta.origin_format = root["origin"].value("format", std::string{});
            meta.origin_count = root["origin"].value("count", 0u);
        }
        return Result<SceneMeta>::success(meta);
    } catch (const std::exception& error) {
        return Result<SceneMeta>::failure(std::string("invalid meta.json: ") + error.what());
    }
}

bool scene_meta_json_is_imported(const std::string& json) {
    try {
        return imported_marker(nlohmann::json::parse(json));
    } catch (const std::exception&) {
        // A file that does not parse is not an imported scene: nothing could
        // read a rotation out of it either.
        return false;
    }
}

Status write_scene_meta(const Path& path, const SceneMeta& meta) {
    const std::string json = scene_meta_to_json(meta);
    return vfs::write_file(path, reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

Result<SceneMeta> read_scene_meta(const Path& path) {
    const Result<std::vector<uint8_t>> bytes = vfs::read_file(path);
    if (!bytes.ok()) return Result<SceneMeta>::failure("cannot open " + path.string());
    const std::string json(bytes.value().begin(), bytes.value().end());
    return scene_meta_from_json(json);
}

Status write_bytes_file(const Path& path, const std::vector<uint8_t>& bytes) {
    return vfs::write_file(path, bytes);
}

SceneFiles scene_files(const Path& folder) {
    return SceneFiles{folder / "original.jpg", folder / "thumb.jpg", folder / "scene.psplat",
                      folder / "meta.json"};
}

Status write_scene_folder(const Path& folder, const SplatScene& scene, const SceneMeta& meta,
                          const std::vector<uint8_t>& original_jpeg,
                          const std::vector<uint8_t>& thumbnail_jpeg) {
    if (original_jpeg.empty()) return Status::error("scene folder needs a non-empty original.jpg");
    if (thumbnail_jpeg.empty()) return Status::error("scene folder needs a non-empty thumb.jpg");

    const Status made = vfs::make_dirs(folder);
    if (!made.ok()) return made;

    const SceneFiles files = scene_files(folder);
    Status status = write_bytes_file(files.original, original_jpeg);
    if (!status.ok()) return status;
    status = write_bytes_file(files.thumbnail, thumbnail_jpeg);
    if (!status.ok()) return status;
    status = write_psplat(files.splat, scene, meta.intrinsics.fx,
                          meta.intrinsics.width, meta.intrinsics.height);
    if (!status.ok()) return status;

    SceneMeta stored = meta;
    stored.splat_count = static_cast<uint32_t>(scene.count());
    return write_scene_meta(files.meta, stored);
}

Result<LoadedScene> read_scene_folder(const Path& folder) {
    const SceneFiles files = scene_files(folder);
    const Result<SceneMeta> meta = read_scene_meta(files.meta);
    if (!meta.ok()) return Result<LoadedScene>::failure(meta.message());

    const Result<SplatScene> scene = read_psplat(files.splat);
    if (!scene.ok()) return Result<LoadedScene>::failure(scene.message());

    LoadedScene loaded;
    loaded.scene = scene.value();
    loaded.meta = meta.value();
    if (loaded.meta.splat_count != 0 && loaded.meta.splat_count != loaded.scene.count()) {
        return Result<LoadedScene>::failure(
            "meta.json splat_count does not match scene.psplat: " +
            std::to_string(loaded.meta.splat_count) + " vs " + std::to_string(loaded.scene.count()));
    }
    loaded.meta.splat_count = static_cast<uint32_t>(loaded.scene.count());
    return Result<LoadedScene>::success(std::move(loaded));
}

Result<LoadedScene> read_scene_path(const Path& path) {
    const std::string extension = lowercase_extension(path);

    // .compressed.ply carries .ply's extension but is a different layout: the
    // header's chunk element decides, so the content is sniffed before the
    // 3DGS reader sees the file.
    if (extension == ".ply" || extension == ".psplat") {
        const Result<std::vector<uint8_t>> probe = read_prefix(path, 4096);
        if (!probe.ok()) return Result<LoadedScene>::failure(probe.message());
        if (sniff_splat_format(path, probe.value().data(), probe.value().size()) ==
            SplatFormat::compressed_ply) {
            return read_splat_file(path);
        }
    }

    LoadedScene loaded;
    if (extension == ".ply") {
        const Status status = import_ply(path, loaded.scene, kMaxSourceSplats);
        if (!status.ok()) return Result<LoadedScene>::failure(status.message());
        const Result<Intrinsics> intrinsics = read_ply_intrinsics(path);
        if (!intrinsics.ok()) return Result<LoadedScene>::failure(intrinsics.message());
        loaded.meta.intrinsics = intrinsics.value();
        loaded.meta.backend = "ply";
        loaded.meta.splat_count = static_cast<uint32_t>(loaded.scene.count());
        return Result<LoadedScene>::success(std::move(loaded));
    }
    if (extension == ".splat" || extension == ".spz") return read_splat_file(path);
    if (extension != ".psplat") {
        return Result<LoadedScene>::failure("unsupported scene file: " + path.string());
    }

    PsplatInfo info;
    auto scene = read_psplat(path, &info);
    if (!scene.ok()) return Result<LoadedScene>::failure(scene.message());
    loaded.scene = scene.take();

    // The header carries the focal length and capture image size; meta.json
    // next to the file can refine them (the scene-folder writer stores the
    // full intrinsics there).
    if (info.focal_px > 0.0f && info.image_width > 0 && info.image_height > 0) {
        loaded.meta.intrinsics.width = info.image_width;
        loaded.meta.intrinsics.height = info.image_height;
        loaded.meta.intrinsics.fx = info.focal_px;
        loaded.meta.intrinsics.fy = info.focal_px;
        loaded.meta.intrinsics.cx = 0.5f * info.image_width;
        loaded.meta.intrinsics.cy = 0.5f * info.image_height;
    }
    const Path meta_path = path.parent_path() / "meta.json";
    vfs::FileStat meta_status;
    if (vfs::stat(meta_path, &meta_status).ok() && meta_status.exists) {
        const Result<SceneMeta> meta = read_scene_meta(meta_path);
        if (meta.ok() && meta.value().intrinsics.fx > 0.0f &&
            meta.value().intrinsics.width > 0 && meta.value().intrinsics.height > 0) {
            loaded.meta = meta.value();
        }
    }
    loaded.meta.format_version = info.version;
    loaded.meta.splat_count = static_cast<uint32_t>(loaded.scene.count());
    return Result<LoadedScene>::success(std::move(loaded));
}

}  // namespace splat
