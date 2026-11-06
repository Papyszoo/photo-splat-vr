// Decoding a photo: content sniff, header guards, the format decoder, then EXIF
// orientation and the manual quarter turns - in that order, so the pixels and
// the intrinsics photo_intrinsics() returns cannot disagree.
//
// The guards run on the header before any allocation: a 70000x1000 PNG is
// refused with its megapixel count instead of trying to allocate it, and a
// body over max_bytes is refused before it is read into memory.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/image.h"
#include "common/path.h"
#include "common/status.h"
#include "image/format.h"

namespace splat {
namespace image {

struct DecodeOptions {
    int max_pixels = 64'000'000;   // 64 MP
    int max_edge = 16'384;
    size_t max_bytes = 512ull << 20;  // 512 MB
    bool apply_orientation = true;
    int rotate_quarters = 0;       // clockwise, applied after the EXIF orientation
    // The format the file's extension names, when the caller knows it. Only
    // used to disambiguate RAW: .nef/.arw/.orf/.rw2/.pef/.srw are plain TIFF
    // container bytes, and .cr3/.raf are ISO-BMFF/Fuji containers with no
    // brand the sniffer knows, so content alone would send them to libtiff.
    ImageFormat hint = ImageFormat::unknown;
};

struct DecodeInfo {
    ImageFormat format = ImageFormat::unknown;
    int source_width = 0, source_height = 0;  // as the file stores them
    int width = 0, height = 0;                // after orientation and rotation
    int exif_orientation = 1;
    int rotate_quarters = 0;
    bool raw_preview = false;
    std::string note;  // "raw preview", "half-size demosaic", ...
};

// The dimensions a header declares, without decoding. Zero when the format has
// no readable header (TGA) or the bytes are too short; decode_photo() then
// falls back to the decoder's own answer.
struct PhotoSize {
    ImageFormat format = ImageFormat::unknown;
    int width = 0;
    int height = 0;
};

PhotoSize probe_photo_size(const uint8_t* data, size_t size);

Result<Image> decode_photo(const uint8_t* data, size_t size, const DecodeOptions& options,
                           DecodeInfo* info);
Result<Image> decode_photo(const Path& path, const DecodeOptions& options, DecodeInfo* info);

// The stb-backed decoder for a JPEG/PNG/BMP/GIF/TGA buffer: the format
// dispatch above uses it, and so does the RAW preview path (an embedded JPEG
// preview decodes with exactly the code the JPEG path uses).
Result<Image> decode_stb_rgb(const uint8_t* data, size_t size);

// Format decoders that need a third-party library. Each is compiled only when
// its SPLAT_HAVE_* macro is defined; decode.cpp dispatches accordingly.
Result<Image> webp_decode_rgb(const uint8_t* data, size_t size, std::string* note);
Result<Image> tiff_decode_rgb(const uint8_t* data, size_t size, std::string* note);
Result<Image> heif_decode_rgb(const uint8_t* data, size_t size, DecodeInfo* info);
Result<Image> raw_decode_rgb(const uint8_t* data, size_t size, DecodeInfo* info);

// The EXIF orientation a HEIF decode should apply. libheif applies the
// container's irot/imir itself, so a file that carries one uses that transform
// only (1 = no EXIF turn); a file without one uses its EXIF tag. Exposed so the
// rule can be tested without an HEVC encoder to author a bespoke file.
int heif_orientation_to_apply(bool had_transform, int exif_orientation);

}  // namespace image
}  // namespace splat
