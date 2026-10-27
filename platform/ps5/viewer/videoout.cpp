// VideoOut presentation for the PS5 viewer title. See videoout.h for why the
// frame is written as 64 KiB tiles from the CPU.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "videoout.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <ps5platform/kernel.h>
#include <ps5platform/videoout.h>

extern "C" int sceKernelUsleep(uint32_t microseconds);

namespace splat {
namespace ps5 {
namespace {

constexpr size_t kTileBytes = 64 * 1024;
constexpr size_t kTileAlignment = 2 * 1024 * 1024;  // VideoOut's buffer alignment
constexpr int kMapProtection =
    PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ |
    PS5_KERNEL_PROT_GPU_WRITE;

double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

std::string code(const char* what, int32_t value) {
    char text[128];
    std::snprintf(text, sizeof(text), "%s -> 0x%08x", what, static_cast<uint32_t>(value));
    return text;
}

bool size_supported(uint32_t width, uint32_t height) {
    return (width == 3840 && height == 2160) || (width == 2560 && height == 1440) ||
           (width == 1920 && height == 1080);
}

// VideoOut's framebuffer bytes: four-byte pixels in 64 KiB tiles of 128x128,
// rounded to its 2 MiB alignment (PS5_Mesa's VIDEOOUT_TILED_BYTES).
size_t tiled_bytes(uint32_t width, uint32_t height) {
    const size_t aligned_width = (static_cast<size_t>(width) + 127u) & ~static_cast<size_t>(127u);
    const size_t aligned_height = (static_cast<size_t>(height) + 127u) & ~static_cast<size_t>(127u);
    const size_t bytes = 4u * aligned_width * aligned_height;
    return (bytes + kTileAlignment - 1u) & ~(kTileAlignment - 1u);
}

// The display reads memory, not the CPU's cache: every line written has to be
// written back before the flip (the same writeback PS5_Mesa's WSI and
// PS5_Vulkan's demo do).
inline void writeback(const void* address, size_t bytes) {
#if defined(__x86_64__) || defined(__i386__)
    const char* at = static_cast<const char*>(address);
    for (size_t offset = 0; offset < bytes; offset += 64) {
        __asm__ volatile("clflushopt (%0)" ::"r"(at + offset) : "memory");
    }
#else
    (void)address;
    (void)bytes;
#endif
}

}  // namespace

VideoOutDisplay::~VideoOutDisplay() {
    close(nullptr);
}

bool VideoOutDisplay::open(const VideoOutConfig& config, std::string& error) {
    if (handle_ >= 0) {
        error = "VideoOut is already open";
        return false;
    }
    if (!size_supported(config.width, config.height)) {
        error = "VideoOut takes 3840x2160, 2560x1440 or 1920x1080 buffers, not " +
                std::to_string(config.width) + "x" + std::to_string(config.height);
        return false;
    }
    if (config.buffers < 2 || config.buffers > 8) {
        error = "buffers must be 2..8";
        return false;
    }
    config_ = config;
    buffer_bytes_ = tiled_bytes(config_.width, config_.height);
    total_bytes_ = buffer_bytes_ * static_cast<size_t>(config_.buffers);

    const int32_t allocated = sceKernelAllocateDirectMemory(
        0, sceKernelGetDirectMemorySize(), total_bytes_, kTileAlignment,
        PS5_KERNEL_DIRECT_TYPE_CPU, &physical_);
    if (allocated != 0) {
        error = code("sceKernelAllocateDirectMemory", allocated) + " for " +
                std::to_string(total_bytes_) + " bytes";
        physical_ = -1;
        return false;
    }
    const int32_t mapped = sceKernelMapDirectMemory(&mapped_, total_bytes_, kMapProtection, 0,
                                                    physical_, kTileAlignment);
    if (mapped != 0) {
        error = code("sceKernelMapDirectMemory", mapped);
        sceKernelReleaseDirectMemory(physical_, total_bytes_);
        physical_ = -1;
        mapped_ = nullptr;
        return false;
    }
    std::memset(mapped_, 0, total_bytes_);
    writeback(mapped_, total_bytes_);

    handle_ = sceVideoOutOpen(PS5_VIDEO_OUT_USER_SYSTEM, 0, 0, nullptr);
    if (handle_ < 0) {
        error = code("sceVideoOutOpen", handle_);
        handle_ = -1;
        sceKernelMunmap(mapped_, total_bytes_);
        mapped_ = nullptr;
        sceKernelReleaseDirectMemory(physical_, total_bytes_);
        physical_ = -1;
        return false;
    }

    const int32_t rate = sceVideoOutSetFlipRate(handle_, config_.flip_rate);
    if (rate != 0) {
        // Not fatal: rate 0 (every vblank) is what a refused rate leaves, and
        // the renderer is slower than the display anyway.
        error = code("sceVideoOutSetFlipRate", rate) + " (continuing at every vblank)";
    }

    ps5_video_out_buffer buffers[8]{};
    for (int i = 0; i < config_.buffers; ++i) {
        buffers[i].data = static_cast<uint8_t*>(mapped_) + static_cast<size_t>(i) * buffer_bytes_;
    }
    uint8_t attribute[PS5_VIDEO_OUT_ATTRIBUTE_BYTES] = {0};
    sceVideoOutSetBufferAttribute2(attribute, PS5_VIDEO_OUT_PIXEL_FORMAT_B8G8R8A8_SDR,
                                   PS5_VIDEO_OUT_TILING_64KB_R_X, config_.width, config_.height, 0,
                                   0, 0);
    const int32_t registered =
        sceVideoOutRegisterBuffers2(handle_, 0, 0, buffers, config_.buffers, attribute, 0, nullptr);
    if (registered != 0) {
        error = code("sceVideoOutRegisterBuffers2", registered);
        sceVideoOutClose(handle_);
        handle_ = -1;
        sceKernelMunmap(mapped_, total_bytes_);
        mapped_ = nullptr;
        sceKernelReleaseDirectMemory(physical_, total_bytes_);
        physical_ = -1;
        return false;
    }
    registered_ = true;
    next_buffer_ = 0;
    flip_argument_ = 0;
    flips_ = 0;
    return true;
}

// VideoOut keeps the buffer of the latest flip shown on screen: PS5_Mesa's
// videoout_free_buffer_locked treats the buffer whose argument == shown as the
// one being scanned out, and only frees buffers flipped before that. So a flip
// `serial` is retired when a LATER flip has been shown, never at `shown ==
// serial`. Every serial handed in here belongs to a flip that was already
// submitted, and serial 0 means "this buffer was never flipped": nothing to
// wait for, which is the state of both buffers on the two first frames.
bool VideoOutDisplay::wait_shown(uint64_t serial) {
    if (handle_ < 0 || serial == 0) return true;
    const double deadline = now_ms() + static_cast<double>(config_.pace_timeout_ms);
    for (;;) {
        uint64_t status[PS5_VIDEO_OUT_FLIP_STATUS_WORDS] = {};
        const int32_t result = sceVideoOutGetFlipStatus(handle_, status);
        if (result != 0) return false;
        if (status[PS5_VIDEO_OUT_FLIP_STATUS_SHOWN_ARGUMENT] > serial) return true;
        if (now_ms() >= deadline) return false;
        sceKernelUsleep(200);
    }
}

bool VideoOutDisplay::present(const void* pixels, double* tile_ms, std::string& error) {
    if (!registered_ || mapped_ == nullptr || handle_ < 0) {
        error = "present without an open VideoOut output";
        return false;
    }
    if (pixels == nullptr) {
        error = "present without pixels";
        return false;
    }
    const int index = next_buffer_;
    uint8_t* frame = static_cast<uint8_t*>(mapped_) + static_cast<size_t>(index) * buffer_bytes_;
    // Reusing a buffer whose flip may still be on screen (or still queued):
    // wait until a later flip has been shown, which is when VideoOut retires
    // it -- never without a bound. The wait is always for a flip that already
    // exists: with `buffers` framebuffers an index comes back `buffers` flips
    // later, so flip `serial + 1` was submitted one present ago (two buffers),
    // and the two first frames wait for nothing (their serial is still 0). A
    // display that stopped reporting falls back to unpaced flips (tearing)
    // instead of a stalled title.
    if (buffer_serial_[index] != 0 && !wait_shown(buffer_serial_[index])) {
        if (!unpaced_reported_) {
            unpaced_reported_ = true;
            error = "a flip could not be confirmed shown within " +
                    std::to_string(config_.pace_timeout_ms) +
                    " ms; presenting unpaced from here (tearing, not a stall)";
        }
    }

    const double started = now_ms();
    const uint32_t width = config_.width;
    const uint32_t height = config_.height;
    const uint32_t* source = static_cast<const uint32_t*>(pixels);
    const uint32_t blocks = (width + 127u) / 128u;
    const uint32_t block_rows = (height + 127u) / 128u;
    // One 64 KiB tile at a time: the destination stays in cache while it is
    // written, and the source rows it reads are contiguous stretches.
    for (uint32_t by = 0; by < block_rows; ++by) {
        const uint32_t rows = (by + 1u) * 128u <= height ? 128u : height - by * 128u;
        for (uint32_t bx = 0; bx < blocks; ++bx) {
            const uint32_t columns = (bx + 1u) * 128u <= width ? 128u : width - bx * 128u;
            uint8_t* tile = frame + (static_cast<size_t>(by) * blocks + bx) * kTileBytes;
            for (uint32_t dy = 0; dy < rows; ++dy) {
                const uint32_t y = by * 128u + dy;
                const uint32_t* row = source + static_cast<size_t>(y) * width;
                // Within a tile the swizzle is the same formula as
                // tile_offset()'s, without the block term, and its result is a
                // byte offset into the tile (the low two bits are the byte
                // within the 4-byte pixel).
                const uint32_t vertical = ((y << 4) & 0x70) ^ ((y << 5) & 0xf00) ^
                                          ((y << 9) & 0x1000) ^ ((y << 8) & 0x4000);
                for (uint32_t dx = 0; dx < columns; ++dx) {
                    const uint32_t x = bx * 128u + dx;
                    const uint32_t off = vertical ^ ((x << 2) & 0xc) ^ ((x << 5) & 0x380) ^
                                         ((x << 4) & 0x400) ^ ((x << 6) & 0x800) ^
                                         ((x << 9) & 0xa000);
                    *reinterpret_cast<uint32_t*>(tile + off) = row[x];
                }
            }
            writeback(tile, kTileBytes);
        }
    }
    __asm__ volatile("sfence" ::: "memory");
    if (tile_ms != nullptr) *tile_ms = now_ms() - started;

    const uint64_t serial = ++flip_argument_;
    const int32_t flipped = sceVideoOutSubmitFlip(handle_, index, PS5_VIDEO_OUT_FLIP_VSYNC,
                                                  static_cast<int64_t>(serial));
    if (flipped != 0) {
        error = code("sceVideoOutSubmitFlip", flipped);
        return false;
    }
    buffer_serial_[index] = serial;
    flips_ = serial;
    next_buffer_ = (index + 1) % config_.buffers;
    return true;
}

void VideoOutDisplay::close(std::string* error) {
    std::string problems;
    if (handle_ >= 0) {
        if (registered_) {
            // Blank first: a stream of black frames, then no more flips. The
            // unregister is only legal once no buffer is on screen, and
            // VideoOut reports busy while one is, so it is retried for 1.2 s.
            sceVideoOutSubmitFlip(handle_, -1, PS5_VIDEO_OUT_FLIP_VSYNC, 0);
            for (int i = 0; i < 200; ++i) {
                const int32_t pending = sceVideoOutIsFlipPending(handle_);
                if (pending <= 0) break;
                sceKernelUsleep(1000);
            }
            int32_t unregistered = PS5_VIDEO_OUT_ERROR_BUSY;
            for (int i = 0; i < 120 && unregistered == PS5_VIDEO_OUT_ERROR_BUSY; ++i) {
                unregistered = sceVideoOutUnregisterBuffers(handle_, 0);
                if (unregistered == PS5_VIDEO_OUT_ERROR_BUSY) sceKernelUsleep(10000);
            }
            if (unregistered == 0) {
                registered_ = false;
            } else {
                problems += code("sceVideoOutUnregisterBuffers", unregistered);
            }
        }
        const int32_t closed = sceVideoOutClose(handle_);
        if (closed != 0) {
            if (!problems.empty()) problems += "; ";
            problems += code("sceVideoOutClose", closed);
        }
        handle_ = -1;
    }
    // The mapped framebuffers are only given back once VideoOut no longer
    // holds them: unmapping memory a flip may still scan out is the one thing
    // this file refuses to do. The process is ending, so leaving them is safe.
    if (mapped_ != nullptr && !registered_) {
        const int32_t unmapped = sceKernelMunmap(mapped_, total_bytes_);
        if (unmapped != 0) {
            if (!problems.empty()) problems += "; ";
            problems += code("sceKernelMunmap", unmapped);
        } else {
            mapped_ = nullptr;
        }
    }
    if (physical_ >= 0 && mapped_ == nullptr) {
        const int32_t released = sceKernelReleaseDirectMemory(physical_, total_bytes_);
        if (released != 0) {
            if (!problems.empty()) problems += "; ";
            problems += code("sceKernelReleaseDirectMemory", released);
        } else {
            physical_ = -1;
        }
    }
    if (error != nullptr) *error = problems;
}

}  // namespace ps5
}  // namespace splat
