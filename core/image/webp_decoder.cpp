// WebP decoding through libwebp (third_party/libwebp).
//
// Lossy, lossless and both alpha modes decode to RGBA in one call; the scene
// has no alpha channel, so the result is composited on white rather than
// truncated (truncating would darken every semi-transparent edge).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <string>

#include <webp/decode.h>

#include "image/decode.h"

namespace splat {
namespace image {

Result<Image> webp_decode_rgb(const uint8_t* data, size_t size, std::string* note) {
    if (data == nullptr || size == 0) return Result<Image>::failure("cannot decode image: empty WebP");

    int width = 0;
    int height = 0;
    if (WebPGetInfo(data, size, &width, &height) == 0 || width <= 0 || height <= 0) {
        return Result<Image>::failure("cannot decode image: the WebP header is not readable");
    }
    if (static_cast<size_t>(width) * static_cast<size_t>(height) > 0x40000000ull) {
        return Result<Image>::failure("cannot decode image: WebP dimensions are out of range");
    }

    uint8_t* rgba = WebPDecodeRGBA(data, size, &width, &height);
    if (rgba == nullptr || width <= 0 || height <= 0) {
        if (rgba != nullptr) WebPFree(rgba);
        return Result<Image>::failure("cannot decode image: the WebP data is truncated or invalid");
    }

    Image image;
    image.resize(width, height);
    const size_t pixels = image.pixel_count();
    for (size_t index = 0; index < pixels; ++index) {
        const uint8_t* from = rgba + index * 4;
        const unsigned alpha = from[3];
        uint8_t* to = image.rgb.data() + index * 3;
        for (int channel = 0; channel < 3; ++channel) {
            to[channel] = static_cast<uint8_t>(
                (static_cast<unsigned>(from[channel]) * alpha + 255u * (255u - alpha) + 127u) / 255u);
        }
    }
    WebPFree(rgba);

    if (note != nullptr) *note = "webp";
    return Result<Image>::success(std::move(image));
}

}  // namespace image
}  // namespace splat
