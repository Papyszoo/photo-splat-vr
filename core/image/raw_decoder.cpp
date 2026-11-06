// RAW decoding through LibRaw (third_party/libraw).
//
// Nearly every camera RAW carries a full- or half-size JPEG preview of the
// frame, and the reconstruction only needs the photo's pixels: the preview is
// therefore tried first (`libraw_unpack_thumb`), and the half-size demosaic is
// the fallback for files without one. `DecodeInfo.raw_preview` and `.note` say
// which path ran, so a scene's provenance is never guessed.
//
// Built with -DLIBRAW_NOTHREADS and without USE_ZLIB/USE_JPEG/USE_LCMS: the
// payload SDK ships none of those libraries, and this path never writes a file
// (no tiff writer, no output profile). A floating-point DNG, which needs zlib,
// is therefore refused by LibRaw with a message instead of decoded.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "image/decode.h"

// A tree without the submodule still builds; decode_photo() then reports that
// RAW support is not in this build.
#if defined(SPLAT_HAVE_RAW)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <libraw/libraw.h>

#include "common/exif.h"
#include "image/orientation.h"

namespace splat {
namespace image {
namespace {

// Below this the preview is too small to reconstruct from; the demosaic path
// gives a better scene (SHARP resizes the input anyway).
constexpr int kMinPreviewEdge = 512;

std::string libraw_error(const char* what, int code) {
    const char* text = libraw_strerror(code);
    return std::string("cannot decode image: ") + what + ": " +
           (text != nullptr ? text : "LibRaw error");
}

}  // namespace

Result<Image> raw_decode_rgb(const uint8_t* data, size_t size, DecodeInfo* info) {
    if (data == nullptr || size == 0) {
        return Result<Image>::failure("cannot decode image: empty RAW buffer");
    }
    libraw_data_t* raw = libraw_init(0);
    if (raw == nullptr) {
        return Result<Image>::failure("cannot decode image: cannot initialise LibRaw");
    }

    int code = libraw_open_buffer(raw, const_cast<uint8_t*>(data), size);
    if (code != LIBRAW_SUCCESS) {
        const std::string message = libraw_error("not a RAW file", code);
        libraw_close(raw);
        return Result<Image>::failure(message);
    }

    const char* label = raw->idata.make[0] != '\0' ? raw->idata.make : "RAW";

    // 1. The embedded preview, decoded by the same stb path a JPEG photo uses.
    code = libraw_unpack_thumb(raw);
    const unsigned int thumb_format = raw->thumbnail.tformat;
    const bool thumb_jpeg = thumb_format == LIBRAW_THUMBNAIL_JPEG;
    const bool thumb_bitmap = thumb_format == LIBRAW_THUMBNAIL_BITMAP;
    if (code == LIBRAW_SUCCESS && raw->thumbnail.thumb != nullptr &&
        raw->thumbnail.tlength > 0 && (thumb_jpeg || thumb_bitmap)) {
        const int preview_width = static_cast<int>(raw->thumbnail.twidth);
        const int preview_height = static_cast<int>(raw->thumbnail.theight);
        const int preview_edge =
            preview_width > preview_height ? preview_width : preview_height;
        if (preview_edge >= kMinPreviewEdge) {
            Result<Image> preview = Result<Image>::failure("raw preview is unreadable");
            if (thumb_jpeg) {
                preview = decode_stb_rgb(reinterpret_cast<const uint8_t*>(raw->thumbnail.thumb),
                                         raw->thumbnail.tlength);
            } else if (preview_width > 0 && preview_height > 0 &&
                       raw->thumbnail.tlength >=
                           static_cast<size_t>(preview_width) * preview_height * 3) {
                Image bitmap;
                bitmap.resize(preview_width, preview_height);
                std::memcpy(bitmap.rgb.data(), raw->thumbnail.thumb, bitmap.byte_size());
                preview = Result<Image>::success(std::move(bitmap));
            }
            if (preview.ok()) {
                Image preview_image = preview.take();
                // LibRaw does not turn an embedded preview, so the preview
                // JPEG's own EXIF orientation is applied here, exactly once.
                // decode_photo() must not apply the container's tag on top.
                const Result<ExifData> exif =
                    thumb_jpeg ? parse_exif(reinterpret_cast<const uint8_t*>(raw->thumbnail.thumb),
                                            raw->thumbnail.tlength)
                               : Result<ExifData>::success(ExifData{});
                if (exif.ok() && exif.value().orientation.has_value()) {
                    apply_exif_orientation(preview_image, *exif.value().orientation);
                }
                if (info != nullptr) {
                    info->raw_preview = true;
                    info->exif_orientation = 1;  // consumed above
                    info->note = std::string("raw preview (") + label + ")";
                    // The size the pixels now have: the orientation is already
                    // in them, so the intrinsics map must not turn them again.
                    info->source_width = preview_image.width;
                    info->source_height = preview_image.height;
                }
                libraw_close(raw);
                return Result<Image>::success(std::move(preview_image));
            }
            // A broken preview is not fatal: fall through to the demosaic.
        }
    }

    // 2. Half-size demosaic. Half size is enough for a reconstruction input and
    // is the cheapest way to get a full frame out of a 40-60 MP sensor.
    raw->params.half_size = 1;
    raw->params.output_bps = 8;
    code = libraw_unpack(raw);
    if (code != LIBRAW_SUCCESS) {
        const std::string message = libraw_error("not a RAW file", code);
        libraw_close(raw);
        return Result<Image>::failure(message);
    }
    code = libraw_dcraw_process(raw);
    if (code != LIBRAW_SUCCESS) {
        const std::string message = libraw_error("cannot process the RAW data", code);
        libraw_close(raw);
        return Result<Image>::failure(message);
    }

    int make_error = LIBRAW_SUCCESS;
    libraw_processed_image_t* processed = libraw_dcraw_make_mem_image(raw, &make_error);
    if (processed == nullptr || make_error != LIBRAW_SUCCESS || processed->type != LIBRAW_IMAGE_BITMAP ||
        processed->colors != 3 || processed->bits != 8) {
        if (processed != nullptr) libraw_dcraw_clear_mem(processed);
        const std::string message =
            make_error != LIBRAW_SUCCESS ? libraw_error("cannot build the image", make_error)
                                         : std::string("cannot decode image: unexpected RAW output");
        libraw_close(raw);
        return Result<Image>::failure(message);
    }

    Image image;
    image.resize(static_cast<int>(processed->width), static_cast<int>(processed->height));
    if (processed->data_size < image.byte_size()) {
        libraw_dcraw_clear_mem(processed);
        libraw_close(raw);
        return Result<Image>::failure("cannot decode image: the RAW image is truncated");
    }
    std::memcpy(image.rgb.data(), processed->data, image.byte_size());
    libraw_dcraw_clear_mem(processed);

    if (info != nullptr) {
        info->raw_preview = false;
        // libraw_dcraw_process() already applied the camera flip (S.flip), so
        // the container's EXIF orientation must not be applied a second time.
        info->exif_orientation = 1;
        info->note = std::string("half-size demosaic (") + label + ")";
        info->source_width = image.width;
        info->source_height = image.height;
    }
    libraw_close(raw);
    return Result<Image>::success(std::move(image));
}

}  // namespace image
}  // namespace splat

#else

// No LibRaw: nothing to define. decode_photo() answers with
// "RAW support is not built into this version".

#endif  // SPLAT_HAVE_RAW
