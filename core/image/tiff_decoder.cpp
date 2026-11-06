// TIFF decoding through libtiff (third_party/libtiff), baseline only.
//
// Built with jpeg/zlib/lzma/zstd/webp/lerc/jbig turned off, so the decoder
// handles none, LZW and PackBits. Anything else is refused *before* libtiff
// opens the file, with a message naming the compression: "TIFF compression
// JPEG is not built in; re-save as baseline TIFF" is a sentence a user can act
// on, a failed read is not.
//
// The client-open callbacks read from the caller's buffer, so a file never has
// to be written to disk and no global state is involved.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <tiffio.h>

#include "common/tiff_tags.h"
#include "image/decode.h"

namespace splat {
namespace image {
namespace {

constexpr uint16_t kTagCompression = 0x0103;

const char* compression_name(uint32_t scheme) {
    switch (scheme) {
        case 2: return "CCITT RLE";
        case 3: return "CCITT Group 3";
        case 4: return "CCITT Group 4";
        case 6: return "old-style JPEG";
        case 7: return "JPEG";
        case 8: return "Deflate";
        case 9: return "JBIG";
        case 32773: return "PackBits (allowed)";
        case 32946: return "Deflate";
        case 32947: return "JBIG-KIT";
        case 34661: return "JBIG";
        case 34712: return "JPEG 2000";
        case 34887: return "LERC";
        case 34925: return "LZMA";
        case 50000: return "Zstd";
        case 50001: return "WebP";
        default: return "an unsupported scheme";
    }
}

bool compression_allowed(uint32_t scheme) {
    return scheme == 1 /*none*/ || scheme == 5 /*LZW*/ || scheme == 32773 /*PackBits*/;
}

// --- the memory client ------------------------------------------------------

struct MemoryFile {
    const uint8_t* data = nullptr;
    size_t size = 0;
    toff_t offset = 0;
};

tmsize_t memory_read(thandle_t handle, void* buffer, tmsize_t count) {
    auto* file = static_cast<MemoryFile*>(handle);
    if (file == nullptr || count <= 0) return 0;
    const size_t wanted = static_cast<size_t>(count);
    if (file->offset >= file->size) return 0;
    const size_t available = file->size - static_cast<size_t>(file->offset);
    const size_t take = wanted < available ? wanted : available;
    std::memcpy(buffer, file->data + file->offset, take);
    file->offset += take;
    return static_cast<tmsize_t>(take);
}

tmsize_t memory_write(thandle_t, void*, tmsize_t) { return 0; }  // read-only

toff_t memory_seek(thandle_t handle, toff_t offset, int whence) {
    auto* file = static_cast<MemoryFile*>(handle);
    if (file == nullptr) return 0;
    toff_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = file->offset; break;
        case SEEK_END: base = static_cast<toff_t>(file->size); break;
        default: return 0;
    }
    const toff_t target = offset;  // toff_t is unsigned; the callers pass forward offsets
    file->offset = base + target;
    if (file->offset > static_cast<toff_t>(file->size)) file->offset = file->size;
    return file->offset;
}

int memory_close(thandle_t) { return 0; }

toff_t memory_size(thandle_t handle) {
    auto* file = static_cast<MemoryFile*>(handle);
    return file == nullptr ? 0 : static_cast<toff_t>(file->size);
}

int memory_map(thandle_t, void**, toff_t*) { return 0; }
void memory_unmap(thandle_t, void*, toff_t) {}

}  // namespace

Result<Image> tiff_decode_rgb(const uint8_t* data, size_t size, std::string* note) {
    if (data == nullptr || size == 0) {
        return Result<Image>::failure("cannot decode image: empty TIFF");
    }

    uint32_t compression = 1;
    if (tiff::read_ifd0_uint(data, size, kTagCompression, &compression) &&
        !compression_allowed(compression)) {
        return Result<Image>::failure(std::string("cannot decode TIFF: compression ") +
                                      compression_name(compression) +
                                      " is not built in; re-save as baseline TIFF");
    }

    MemoryFile file;
    file.data = data;
    file.size = size;
    TIFF* tiff = TIFFClientOpen("memory", "r", &file, memory_read, memory_write, memory_seek,
                                memory_close, memory_size, memory_map, memory_unmap);
    if (tiff == nullptr) return Result<Image>::failure("cannot decode image: not a readable TIFF");

    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    if (width == 0 || height == 0) {
        TIFFClose(tiff);
        return Result<Image>::failure("cannot decode image: the TIFF has no pixels");
    }
    // libtiff's own answer, in case the pre-open scan missed a chained IFD.
    uint16_t strip_compression = 1;
    if (TIFFGetField(tiff, TIFFTAG_COMPRESSION, &strip_compression) &&
        !compression_allowed(strip_compression)) {
        const std::string reason = std::string("cannot decode TIFF: compression ") +
                                   compression_name(strip_compression) +
                                   " is not built in; re-save as baseline TIFF";
        TIFFClose(tiff);
        return Result<Image>::failure(reason);
    }

    // libtiff normalises the raster to TIFFReadRGBAImageOriented's requested
    // orientation by itself, reading TIFFTAG_ORIENTATION. Declare the stored
    // raster already top-left so the pixels come out as stored: the EXIF
    // orientation is applied once, by decode_photo(), from the same IFD0 tag.
    TIFFSetField(tiff, TIFFTAG_ORIENTATION, static_cast<uint16_t>(ORIENTATION_TOPLEFT));
    std::vector<uint32_t> raster(static_cast<size_t>(width) * height, 0);
    if (TIFFReadRGBAImageOriented(tiff, width, height, raster.data(), ORIENTATION_TOPLEFT, 0) == 0) {
        TIFFClose(tiff);
        return Result<Image>::failure("cannot decode image: the TIFF strip data is unreadable");
    }
    TIFFClose(tiff);

    Image image;
    image.resize(static_cast<int>(width), static_cast<int>(height));
    for (size_t index = 0; index < raster.size(); ++index) {
        const uint32_t pixel = raster[index];
        const unsigned alpha = TIFFGetA(pixel);
        uint8_t* to = image.rgb.data() + index * 3;
        const uint8_t channels[3] = {static_cast<uint8_t>(TIFFGetR(pixel)),
                                     static_cast<uint8_t>(TIFFGetG(pixel)),
                                     static_cast<uint8_t>(TIFFGetB(pixel))};
        for (int channel = 0; channel < 3; ++channel) {
            to[channel] = static_cast<uint8_t>(
                (static_cast<unsigned>(channels[channel]) * alpha + 255u * (255u - alpha) + 127u) /
                255u);
        }
    }

    if (note != nullptr) {
        *note = compression_allowed(compression) && compression == 5 ? "tiff (LZW)" : "tiff";
    }
    return Result<Image>::success(std::move(image));
}

}  // namespace image
}  // namespace splat
