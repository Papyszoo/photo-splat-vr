#include "scene/scene_folder.h"

#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "scene/ply.h"

namespace splat {
namespace {

Status write_bytes(const Path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error("cannot open " + path.string() + " for writing");
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) return Status::error("failed while writing " + path.string());
    return Status::success();
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
        return Result<SceneMeta>::success(meta);
    } catch (const std::exception& error) {
        return Result<SceneMeta>::failure(std::string("invalid meta.json: ") + error.what());
    }
}

Status write_scene_meta(const Path& path, const SceneMeta& meta) {
    const std::string json = scene_meta_to_json(meta);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status::error("cannot open " + path.string() + " for writing");
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!out) return Status::error("failed while writing " + path.string());
    return Status::success();
}

Result<SceneMeta> read_scene_meta(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<SceneMeta>::failure("cannot open " + path.string());
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return scene_meta_from_json(json);
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

    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return Status::error("cannot create " + folder.string() + ": " + error.message());

    const SceneFiles files = scene_files(folder);
    Status status = write_bytes(files.original, original_jpeg);
    if (!status.ok()) return status;
    status = write_bytes(files.thumbnail, thumbnail_jpeg);
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
    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    LoadedScene loaded;
    if (extension == ".ply") {
        const Status status = import_ply(path, loaded.scene);
        if (!status.ok()) return Result<LoadedScene>::failure(status.message());
        const Result<Intrinsics> intrinsics = read_ply_intrinsics(path);
        if (!intrinsics.ok()) return Result<LoadedScene>::failure(intrinsics.message());
        loaded.meta.intrinsics = intrinsics.value();
        loaded.meta.backend = "ply";
        loaded.meta.splat_count = static_cast<uint32_t>(loaded.scene.count());
        return Result<LoadedScene>::success(std::move(loaded));
    }
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
    std::error_code exists_error;
    if (std::filesystem::exists(meta_path, exists_error) && !exists_error) {
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
