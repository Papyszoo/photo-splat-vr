#include "jobs/image_io.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include "common/exif.h"

// Static implementations keep the stb symbols local to this translation unit;
// tests and other components may include the same headers with their own
// implementation.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

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

Result<std::vector<uint8_t>> read_file_bytes(const Path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return Result<std::vector<uint8_t>>::failure("cannot open " + path.string());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (file.bad()) return Result<std::vector<uint8_t>>::failure("failed while reading " + path.string());
    return Result<std::vector<uint8_t>>::success(std::move(bytes));
}

bool looks_like_jpeg(const uint8_t* data, size_t size) {
    return data != nullptr && size >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

Result<Image> decode_image(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return Result<Image>::failure("image buffer is empty");
    if (size > static_cast<size_t>(INT32_MAX)) return Result<Image>::failure("image is too large");

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, 3);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return Result<Image>::failure(std::string("cannot decode image: ") +
                                      (reason != nullptr ? reason : "unknown format"));
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return Result<Image>::failure("decoded image has no pixels");
    }

    Image image;
    image.resize(width, height);
    std::copy(pixels, pixels + image.byte_size(), image.rgb.begin());
    stbi_image_free(pixels);
    return Result<Image>::success(std::move(image));
}

Result<Image> decode_image(const Path& path) {
    const Result<std::vector<uint8_t>> bytes = read_file_bytes(path);
    if (!bytes.ok()) return Result<Image>::failure(bytes.message());
    return decode_image(bytes.value().data(), bytes.value().size());
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

}  // namespace splat
