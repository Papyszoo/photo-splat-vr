// HEIC/HEIF and AVIF decoding through libheif (third_party/libheif), which
// brings its own HEVC decoder (libde265) and AV1 decoder (libaom).
//
// Two rules make this decoder different from the others:
//
// 1. The size guard runs on the handle's ispe box *before* any decode, because
//    libheif would otherwise allocate the full image first.
// 2. libheif applies the container's own irot/imir transforms while decoding.
//    The EXIF orientation tag is a *second* description of the same rotation in
//    files that carry both, so applying it on top of an applied transform turns
//    the photo twice. The rule is: a file with a transform property uses the
//    transform only; a file without one uses its EXIF orientation.
//
// The EXIF block travels as an item: four big-endian bytes holding the offset
// to the TIFF header, then the Exif block itself.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "image/decode.h"

// A tree without the codec prefix (no third_party/build_codecs.sh run) still
// builds: this translation unit then only carries the documentation and the
// orientation rule, and decode_photo() reports that HEIF support is not built.
#if defined(SPLAT_HAVE_HEIF)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <libheif/heif.h>
#include <libheif/heif_properties.h>

#include "common/exif.h"

namespace splat {
namespace image {
namespace {

// Whether the item carries an irot or an imir property, i.e. whether libheif
// will turn the pixels itself. clap (cropping) does not count.
bool has_orientation_transform(const heif_context* context, heif_item_id id) {
    const int count = heif_item_get_transformation_properties(context, id, nullptr, 0);
    if (count <= 0) return false;
    std::vector<heif_property_id> properties(static_cast<size_t>(count));
    const int filled =
        heif_item_get_transformation_properties(context, id, properties.data(), count);
    for (int index = 0; index < filled; ++index) {
        const heif_item_property_type type =
            heif_item_get_property_type(context, id, properties[static_cast<size_t>(index)]);
        if (type == heif_item_property_type_transform_rotation ||
            type == heif_item_property_type_transform_mirror) {
            return true;
        }
    }
    return false;
}

// The TIFF/Exif blob inside a HEIF Exif item, or empty when the offset does not
// point anywhere sensible.
std::vector<uint8_t> exif_tiff_bytes(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4) return {};
    size_t offset = (static_cast<size_t>(data[0]) << 24) | (static_cast<size_t>(data[1]) << 16) |
                    (static_cast<size_t>(data[2]) << 8) | data[3];
    if (offset >= size) offset = 4;  // most writers mean "right after the offset"
    if (offset + 8 > size) return {};
    // Some writers put the JPEG APP1 magic in front of the TIFF header.
    if (offset + 6 <= size && std::memcmp(data + offset, "Exif\0\0", 6) == 0) offset += 6;
    if (offset + 8 > size) return {};
    return std::vector<uint8_t>(data + offset, data + size);
}

}  // namespace

int heif_orientation_to_apply(bool had_transform, int exif_orientation) {
    if (exif_orientation < 1 || exif_orientation > 8) exif_orientation = 1;
    return had_transform ? 1 : exif_orientation;
}

Result<Image> heif_decode_rgb(const uint8_t* data, size_t size, DecodeInfo* info) {
    const char* label =
        info != nullptr && info->format == ImageFormat::avif ? "AVIF" : "HEIC/HEIF";

    heif_context* context = heif_context_alloc();
    if (context == nullptr) return Result<Image>::failure("cannot decode image: no libheif context");

    heif_error error = heif_context_read_from_memory_without_copy(context, data, size, nullptr);
    if (error.code != heif_error_Ok) {
        const std::string message =
            std::string("cannot decode image: ") + label + ": " +
            (error.message != nullptr ? error.message : "the container is not readable");
        heif_context_free(context);
        return Result<Image>::failure(message);
    }

    heif_image_handle* handle = nullptr;
    error = heif_context_get_primary_image_handle(context, &handle);
    if (error.code != heif_error_Ok || handle == nullptr) {
        const std::string message = std::string("cannot decode image: ") + label + ": " +
                                    (error.message != nullptr ? error.message : "no primary image");
        heif_context_free(context);
        return Result<Image>::failure(message);
    }

    // The guard runs on the stored (ispe) size, before the decoder allocates.
    const int ispe_width = heif_image_handle_get_ispe_width(handle);
    const int ispe_height = heif_image_handle_get_ispe_height(handle);
    if (ispe_width <= 0 || ispe_height <= 0 ||
        static_cast<uint64_t>(ispe_width) * static_cast<uint64_t>(ispe_height) > 0x40000000ull) {
        heif_image_handle_release(handle);
        heif_context_free(context);
        return Result<Image>::failure(std::string("cannot decode image: ") + label +
                                      " dimensions are out of range");
    }

    heif_image* image = nullptr;
    error = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGB,
                              nullptr);
    if (error.code != heif_error_Ok || image == nullptr) {
        // Which built-in decoder plugins this archive really carries: without
        // this, "not built in" cannot be told apart from a missing plugin.
        const int hevc_plugins = heif_get_decoder_descriptors(heif_compression_HEVC, nullptr, 0);
        const int av1_plugins = heif_get_decoder_descriptors(heif_compression_AV1, nullptr, 0);
        const std::string message =
            std::string("cannot decode image: ") + label + ": " +
            (error.message != nullptr ? error.message : "decode failed") +
            " (decoder plugins: HEVC " + std::to_string(hevc_plugins) + ", AV1 " +
            std::to_string(av1_plugins) + ")";
        if (image != nullptr) heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(context);
        return Result<Image>::failure(message);
    }

    const int width = heif_image_get_width(image, heif_channel_interleaved);
    const int height = heif_image_get_height(image, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
    if (plane == nullptr || width <= 0 || height <= 0 || stride < width * 3) {
        heif_image_release(image);
        heif_image_handle_release(handle);
        heif_context_free(context);
        return Result<Image>::failure(std::string("cannot decode image: ") + label +
                                      " produced no pixels");
    }

    Image decoded;
    decoded.resize(width, height);
    for (int y = 0; y < height; ++y) {
        std::memcpy(decoded.pixel(0, y), plane + static_cast<size_t>(y) * static_cast<size_t>(stride),
                    static_cast<size_t>(width) * 3);
    }

    // The EXIF orientation: skipped when the container's own transform already
    // turned the pixels (see the file comment).
    int exif_orientation = 1;
    const bool transformed = has_orientation_transform(context, heif_image_handle_get_item_id(handle));
    if (heif_image_handle_get_number_of_metadata_blocks(handle, "Exif") > 0) {
        heif_item_id metadata_id = 0;
        if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &metadata_id, 1) >= 1) {
            const size_t metadata_size = heif_image_handle_get_metadata_size(handle, metadata_id);
            if (metadata_size >= 8) {
                std::vector<uint8_t> blob(metadata_size);
                const heif_error metadata_error =
                    heif_image_handle_get_metadata(handle, metadata_id, blob.data());
                if (metadata_error.code == heif_error_Ok) {
                    const std::vector<uint8_t> tiff =
                        exif_tiff_bytes(blob.data(), blob.size());
                    if (!tiff.empty()) {
                        const Result<ExifData> exif = parse_exif(tiff.data(), tiff.size());
                        if (exif.ok() && exif.value().orientation.has_value()) {
                            exif_orientation = *exif.value().orientation;
                        }
                    }
                }
            }
        }
    }

    if (info != nullptr) {
        info->exif_orientation = heif_orientation_to_apply(transformed, exif_orientation);
        info->note = transformed ? std::string(label) + " (container rotation applied)"
                                 : std::string(label);
    }

    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(context);
    return Result<Image>::success(std::move(decoded));
}

}  // namespace image
}  // namespace splat

#else

// Without libheif the rule is still the rule; only the decoder is missing.
namespace splat {
namespace image {

int heif_orientation_to_apply(bool had_transform, int exif_orientation) {
    if (exif_orientation < 1 || exif_orientation > 8) exif_orientation = 1;
    return had_transform ? 1 : exif_orientation;
}

}  // namespace image
}  // namespace splat

#endif  // SPLAT_HAVE_HEIF
