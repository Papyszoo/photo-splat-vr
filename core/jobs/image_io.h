#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "common/status.h"

namespace splat {

// Ingest image I/O, implemented with the stb headers in third_party. Decoding
// always yields tightly packed RGB8 (no alpha), matching Image.

Result<std::vector<uint8_t>> read_file_bytes(const Path& path);

// True for a JPEG SOI marker. Used to keep the source JPEG verbatim when the
// photo already is one, and to decide whether EXIF parsing makes sense.
bool looks_like_jpeg(const uint8_t* data, size_t size);

Result<Image> decode_image(const uint8_t* data, size_t size);
Result<Image> decode_image(const Path& path);

// quality is clamped to [1, 100]. Returns the encoded bytes.
Result<std::vector<uint8_t>> encode_jpeg(const Image& image, int quality = 90);

// Encodes straight RGBA8 (row-major, w*h*4 bytes) as a PNG. UI screens use it
// for UiCanvas::write_png and the console frame dump; decode_image() reads it
// back as RGB. Fails on a null buffer or a non-positive size.
Result<std::vector<uint8_t>> encode_png(const uint8_t* rgba, int w, int h);

// Scales down so the longest edge is at most max_edge, keeping the aspect
// ratio. Images already small enough are returned unchanged; never upscales.
Result<Image> make_thumbnail(const Image& image, int max_edge = 512);

// SHARP's focal length in original-image pixels for a photo: the EXIF value
// when the bytes are a JPEG carrying a usable focal length, otherwise the 30 mm
// default (common/exif.h). A malformed Exif segment falls back to the default
// instead of failing, so one bad tag cannot fail a conversion. The job runner
// and the console convert mode both call this.
Result<float> focal_px_from_photo(const uint8_t* data, size_t size, int width, int height);

}  // namespace splat
