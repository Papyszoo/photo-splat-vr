#include "jobs/image_io.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "common/exif.h"
#include "image/orientation.h"
#include "vfs/io.h"

// Static implementations keep the stb symbols local to this translation unit;
// tests and other components may include the same headers with their own
// implementation. Decoding itself is core/image/decode.cpp.
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace splat {
namespace {

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* first = static_cast<const uint8_t*>(data);
    out->insert(out->end(), first, first + size);
}

}  // namespace

Result<std::vector<uint8_t>> read_file_bytes(const Path& path, size_t max_bytes) {
    // One reader for the whole app (L3): the VFS picks the local disk or a
    // network location, and its size guard runs before anything is allocated.
    return vfs::read_file(path, max_bytes);
}

bool looks_like_jpeg(const uint8_t* data, size_t size) {
    return data != nullptr && size >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

Result<Image> decode_image(const uint8_t* data, size_t size) {
    return image::decode_photo(data, size, image::DecodeOptions{}, nullptr);
}

Result<Image> decode_image(const Path& path) { return image::decode_photo(path, {}, nullptr); }

Result<Image> decode_image(const Path& path, image::DecodeInfo* info) {
    return image::decode_photo(path, {}, info);
}

Result<std::vector<uint8_t>> encode_jpeg(const Image& image, int quality) {
    if (image.empty()) return Result<std::vector<uint8_t>>::failure("cannot encode an empty image");
    quality = std::clamp(quality, 1, 100);

    std::vector<uint8_t> bytes;
    bytes.reserve(image.byte_size() / 8 + 1024);
    if (stbi_write_jpg_to_func(append_bytes, &bytes, image.width, image.height, 3, image.rgb.data(),
                               quality) == 0) {
        return Result<std::vector<uint8_t>>::failure("JPEG encoding failed");
    }
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

Result<std::vector<uint8_t>> encode_png(const uint8_t* rgba, int w, int h) {
    if (rgba == nullptr) return Result<std::vector<uint8_t>>::failure("cannot encode an empty image");
    if (w <= 0 || h <= 0) return Result<std::vector<uint8_t>>::failure("image has no pixels");

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) / 8 + 1024);
    if (stbi_write_png_to_func(append_bytes, &bytes, w, h, 4, rgba, w * 4) == 0) {
        return Result<std::vector<uint8_t>>::failure("PNG encoding failed");
    }
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

Result<Image> make_thumbnail(const Image& image, int max_edge) {
    if (image.empty()) return Result<Image>::failure("cannot thumbnail an empty image");
    if (max_edge <= 0) return Result<Image>::failure("thumbnail edge must be positive");

    const int longest = std::max(image.width, image.height);
    if (longest <= max_edge) return Result<Image>::success(image);

    const double scale = static_cast<double>(max_edge) / static_cast<double>(longest);
    const int width = std::max(1, static_cast<int>(std::lround(image.width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(image.height * scale)));

    Image thumbnail;
    thumbnail.resize(width, height);
    if (stbir_resize_uint8_srgb(image.rgb.data(), image.width, image.height, 0,
                                thumbnail.rgb.data(), width, height, 0, STBIR_RGB) == nullptr) {
        return Result<Image>::failure("thumbnail resize failed");
    }
    return Result<Image>::success(std::move(thumbnail));
}

Result<float> focal_px_from_photo(const uint8_t* data, size_t size, int width, int height) {
    if (looks_like_jpeg(data, size)) {
        const Result<float> exif = focal_px_from_jpeg(data, size, width, height);
        if (exif.ok()) return exif;
    }
    return focal_px_from_exif(ExifFocal{}, width, height);
}

Result<Intrinsics> photo_intrinsics(const uint8_t* data, size_t size,
                                    const image::DecodeInfo& info) {
    if (info.width <= 0 || info.height <= 0) {
        return Result<Intrinsics>::failure("decoded photo has no size");
    }
    const int source_w = info.source_width > 0 ? info.source_width : info.width;
    const int source_h = info.source_height > 0 ? info.source_height : info.height;

    // A malformed Exif segment falls back to the 30 mm default, exactly as
    // focal_px_from_photo() does for the console convert mode.
    ExifFocal focal_data;
    const Result<ExifData> exif = parse_exif(data, size);
    if (exif.ok()) focal_data = exif.value().focal;
    const Result<float> focal = focal_px_from_exif(focal_data, source_w, source_h);
    if (!focal.ok()) return Result<Intrinsics>::failure(focal.message());

    image::ImageIntrinsics in;
    in.width = source_w;
    in.height = source_h;
    in.fx = focal.value();
    in.fy = focal.value();
    in.cx = static_cast<float>(source_w) / 2.0f;
    in.cy = static_cast<float>(source_h) / 2.0f;

    // The same map the pixels went through; no resampling happens, so a scale
    // of 1 is the normal case and only a decoder that resized needs the ratio.
    image::ImageIntrinsics out =
        image::orient_intrinsics(in, info.exif_orientation, info.rotate_quarters);
    if (out.width != info.width || out.height != info.height) {
        // A decoder applied a transform of its own (libheif's irot/imir). The
        // decoded size is then the truth, and the map above was computed for a
        // frame it did not know about: use the centre of the decoded image,
        // which is where a centred capture's principal point lands under any
        // 90-degree turn (within a pixel).
        out.width = info.width;
        out.height = info.height;
        out.cx = static_cast<float>(info.width) / 2.0f;
        out.cy = static_cast<float>(info.height) / 2.0f;
    }
    const float long_source = static_cast<float>(std::max(source_w, source_h));
    const float long_out = static_cast<float>(std::max(info.width, info.height));
    const float scale = long_source > 0.0f ? long_out / long_source : 1.0f;

    Intrinsics intrinsics;
    intrinsics.width = static_cast<uint32_t>(info.width);
    intrinsics.height = static_cast<uint32_t>(info.height);
    intrinsics.fx = out.fx * scale;
    intrinsics.fy = out.fy * scale;
    intrinsics.cx = out.cx * scale;
    intrinsics.cy = out.cy * scale;
    return Result<Intrinsics>::success(intrinsics);
}

}  // namespace splat
