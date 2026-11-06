#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/image.h"
#include "common/path.h"
#include "common/status.h"
#include "image/decode.h"
#include "scene/scene_folder.h"

namespace splat {

// Ingest image I/O. The decoders live in core/image; these are the ingest-side
// wrappers, and decoding always yields tightly packed RGB8 (no alpha), matching
// Image.

// Reads a whole file. `max_bytes` (0 = no limit) is checked against the file
// size before anything is read, so an oversized photo is refused without
// allocating it - the guard decode_photo() runs afterwards is too late for a
// multi-GB file.
Result<std::vector<uint8_t>> read_file_bytes(const Path& path, size_t max_bytes = 0);

// True for a JPEG SOI marker. Used to keep the source JPEG verbatim when the
// photo already is one, and to decide whether EXIF parsing makes sense.
bool looks_like_jpeg(const uint8_t* data, size_t size);

Result<Image> decode_image(const uint8_t* data, size_t size);
Result<Image> decode_image(const Path& path);
// The same decode, reporting the format, the source size, the EXIF orientation
// and the decoded size the caller needs for the intrinsics.
Result<Image> decode_image(const Path& path, image::DecodeInfo* info);

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

// The intrinsics a finished scene carries: the EXIF focal (or the 30 mm
// default) for the size the file stores, then the same EXIF-orientation and
// quarter-turn map the pixels went through, so the scene and its photo always
// agree. `info` comes from decode_image()/image::decode_photo().
Result<Intrinsics> photo_intrinsics(const uint8_t* data, size_t size,
                                    const image::DecodeInfo& info);

}  // namespace splat
