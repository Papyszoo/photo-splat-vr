#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "common/path.h"
#include "common/status.h"

namespace splat {

// EXIF focal data from a JPEG's APP1/Exif segment. Both fields are optional:
// cameras often write only one of them, and screenshots write neither.
struct ExifFocal {
    std::optional<double> focal_length_mm;      // EXIF FocalLength (0x920A)
    std::optional<double> focal_length_35mm;    // EXIF FocalLengthIn35mmFilm (0xA405)
};

// Everything the decoder needs from the metadata: the orientation tag and the
// focal lengths. Both are optional; a photo without EXIF is not an error.
struct ExifData {
    std::optional<int> orientation;  // EXIF Orientation (0x0112), only 1..8
    ExifFocal focal;
};

// Reads the Exif block of a JPEG (APP1) or a TIFF/RAW file's IFD0. A file with
// no EXIF at all (a PNG, a JPEG without APP1) returns empty data, not an error;
// a malformed EXIF structure is an error and the caller falls back to defaults.
Result<ExifData> parse_exif(const uint8_t* data, size_t size);

// Applies `quarters` clockwise 90-degree turns to the *displayed* image a
// starting orientation describes (negative quarters turn counter-clockwise)
// and returns the orientation tag the file then needs. The eight EXIF values
// are closed under rotation; values outside 1..8 are treated as 1.
int turn_exif_orientation(int orientation, int quarters);

// Rewrites the IFD0 Orientation tag (0x0112) of a JPEG (its APP1/Exif block) or
// a baseline TIFF so it reads `orientation`. The pixel data is not re-encoded:
// an existing SHORT/LONG entry is patched in place, a missing one is added by
// appending a copy of IFD0 and pointing the header at it, and a JPEG with no
// Exif APP1 gets a minimal segment. `file` is returned unchanged when it is
// neither a JPEG nor a TIFF, or when the structure cannot be rewritten safely;
// the caller then keeps the source file and saves the turn instead.
Result<std::vector<uint8_t>> set_exif_orientation(const std::vector<uint8_t>& file,
                                                  int orientation);

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
