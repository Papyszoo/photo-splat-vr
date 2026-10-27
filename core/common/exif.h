#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// EXIF focal data from a JPEG's APP1/Exif segment. Both fields are optional:
// cameras often write only one of them, and screenshots write neither.
struct ExifFocal {
    std::optional<double> focal_length_mm;      // EXIF FocalLength (0x920A)
    std::optional<double> focal_length_35mm;    // EXIF FocalLengthIn35mmFilm (0xA405)
};

// SHARP's fallback when no EXIF focal length is present (docs/research/
// sharp-reference.md, Q3).
inline constexpr double kDefaultFocal35mm = 30.0;
inline constexpr double kFocalBelow10mmScale = 8.4;

// Parses the Exif segment. Returns an empty ExifFocal when the file is a valid
// JPEG without one, and an error for a malformed JPEG/Exif structure.
Result<ExifFocal> parse_jpeg_exif_focal(const uint8_t* data, size_t size);
Result<ExifFocal> parse_jpeg_exif_focal(const Path& path);

// f_px = f_mm * sqrt(w^2 + h^2) / sqrt(36^2 + 24^2) for 35 mm film.
float focal_px_from_mm(double focal_mm, int width, int height);

// Applies SHARP's preference order: FocalLengthIn35mmFilm, then FocalLength
// (scaled by 8.4 when below 10 mm), then the 30 mm default.
Result<float> focal_px_from_exif(const ExifFocal& exif, int width, int height);

// Convenience: parse the JPEG and compute f_px with the rules above.
Result<float> focal_px_from_jpeg(const uint8_t* data, size_t size, int width, int height);
Result<float> focal_px_from_jpeg(const Path& path, int width, int height);

}  // namespace splat
