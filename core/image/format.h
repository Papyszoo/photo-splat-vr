// The one photo format list. Every component that decides "is this a photo"
// or "which decoder owns these bytes" goes through this header: the gallery
// scan, the inbox watcher, the capture helper, the upload store and the bench
// converter all had their own extension check before L1.
//
// Sniffing is content first (magic bytes); the extension only decides between
// TIFF and a RAW container, because those share the TIFF header.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"

namespace splat {

enum class ImageFormat { unknown, jpeg, png, webp, heif, avif, tiff, bmp, gif, tga, raw };

// "jpeg", "png", ... and "unknown". Lowercase, stable, used in DecodeInfo and
// in the photos screen's second caption.
const char* image_format_name(ImageFormat format);

// The extensions a photo may have, lowercase and with the dot:
// .jpg .jpeg .png .webp .heic .heif .avif .tif .tiff .bmp .gif .tga
// .dng .cr2 .cr3 .nef .arw .raf .orf .rw2 .pef .srw
const std::vector<std::string>& photo_extensions();

// Extension in photo_extensions(), case-insensitive. This says nothing about
// the content: a .jpg full of text still is a photo file to a directory scan,
// and the decode then fails with a message.
bool is_photo_file(const Path& path);

// unknown for anything not in photo_extensions(). TIFF and the RAW containers
// are told apart here: a .dng/.cr2/... is raw, a .tif/.tiff is tiff.
ImageFormat image_format_from_extension(const Path& path);

// Magic bytes only, so an upload can be named and routed before its extension
// means anything. `raw` is a TIFF whose IFD0 carries the DNG tag, or a CR2.
ImageFormat sniff_image_format(const uint8_t* data, size_t size);

}  // namespace splat
