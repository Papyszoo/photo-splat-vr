#include "gallery/thumbs.h"

#include <cstddef>
#include <filesystem>
#include <system_error>

#include "jobs/image_io.h"
#include "scene/scene_folder.h"

namespace splat {
namespace {

RgbaImage to_rgba(const Image& image) {
    RgbaImage out;
    out.w = static_cast<uint32_t>(image.width);
    out.h = static_cast<uint32_t>(image.height);
    out.rgba.resize(out.pixel_count() * 4);
    for (size_t i = 0; i < out.pixel_count(); ++i) {
        out.rgba[i * 4 + 0] = image.rgb[i * 3 + 0];
        out.rgba[i * 4 + 1] = image.rgb[i * 3 + 1];
        out.rgba[i * 4 + 2] = image.rgb[i * 3 + 2];
        out.rgba[i * 4 + 3] = 255;
    }
    return out;
}

Result<RgbaImage> load_fitted(const Path& path, int max_edge) {
    if (max_edge <= 0) return Result<RgbaImage>::failure("thumbnail edge must be positive");

    const Result<Image> decoded = decode_image(path);
    if (!decoded.ok()) return Result<RgbaImage>::failure(decoded.message());

    const Result<Image> fitted = make_thumbnail(decoded.value(), max_edge);
    if (!fitted.ok()) return Result<RgbaImage>::failure(fitted.message());
    return Result<RgbaImage>::success(to_rgba(fitted.value()));
}

}  // namespace

Result<RgbaImage> load_photo_thumb(const Path& path, int max_edge) {
    return load_fitted(path, max_edge);
}

Result<RgbaImage> load_scene_thumb(const Path& folder, int max_edge) {
    const Path thumb = scene_files(folder).thumbnail;
    std::error_code error;
    if (!std::filesystem::is_regular_file(thumb, error) || error) {
        return Result<RgbaImage>::failure("scene has no thumb.jpg: " + folder.string());
    }
    return load_fitted(thumb, max_edge);
}

}  // namespace splat
