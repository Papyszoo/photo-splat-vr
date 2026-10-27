// The TV, through VideoOut, for the viewer title.
//
// A title has no Vulkan swapchain on the console: RADV's PS5 winsys has no WSI
// (docs/research/spike-b-memory.md section 3, and PS5_Mesa's
// src/vulkan/wsi/wsi_common_videoout.c is Mesa-internal, reachable only through
// ps5-opengl's EGL). What a title can do, and what PS5LM's GL stack does under
// it, is the same pair of things:
//
//   1. allocate a few framebuffers of direct memory (CPU and GPU share the
//      address), register them with libSceVideoOut and hand a flip its index;
//   2. lay each frame out the way the display scans it: 64 KiB tiles of
//      128x128 four-byte pixels, no pipe or bank swizzle (VideoOut's tiling 0,
//      ADDR_SW_64KB_R_X). PS5_Mesa's WSI writes the same tiles from the CPU
//      (wsi_common_videoout.c) and PS5_Vulkan's demo renderer does it too
//      (src/demo_renderer.cpp, whose tiled() formula this copies).
//
// The renderer draws into a RADV image, not into the registered buffer: RADV
// lays a plain VkImage out in whatever swizzle addrlib picks, and the
// display-swizzle layout is only reachable through Mesa's private scanout flag.
// So the viewer copies the finished frame through a host-visible RADV buffer
// (GPU -> CPU) and writes the tiles here. The copy is measured and logged;
// replacing it with a compute pass that writes the tiles on the GPU is the
// follow-up in docs/BACKLOG.md.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace splat {
namespace ps5 {

struct VideoOutConfig {
    // VideoOut takes buffers of 3840x2160, 2560x1440 and 1920x1080 only; it
    // scales one of those to fill the screen.
    uint32_t width = 1920;
    uint32_t height = 1080;
    // 0 = a flip may show every vblank, 1 = every second, 2 = every third
    // (sceVideoOutSetFlipRate). 1 paces a 60 Hz output to 30 fps.
    int flip_rate = 1;
    // Two is enough to render while one is on screen; VideoOut's WSI registers
    // five because it also keeps them across devices.
    int buffers = 2;
    // How long a present may wait for an earlier flip of the same buffer to
    // show before it stops pacing (tearing instead of a stall).
    uint64_t pace_timeout_ms = 500;
};

// One open VideoOut output with its registered framebuffers. Not copyable.
class VideoOutDisplay {
public:
    VideoOutDisplay() = default;
    ~VideoOutDisplay();
    VideoOutDisplay(const VideoOutDisplay&) = delete;
    VideoOutDisplay& operator=(const VideoOutDisplay&) = delete;

    // Allocates the direct-memory framebuffers, opens VideoOut and registers
    // them. On failure `error` says which call failed and with what code, and
    // nothing is left allocated.
    bool open(const VideoOutConfig& config, std::string& error);

    uint32_t width() const { return config_.width; }
    uint32_t height() const { return config_.height; }
    // Bytes of one framebuffer (VideoOut's tile layout, 2 MiB aligned).
    size_t buffer_bytes() const { return buffer_bytes_; }
    int handle() const { return handle_; }
    bool registered() const { return registered_; }
    uint64_t flips() const { return flips_; }

    // Copies one frame into the next framebuffer and flips it.
    //
    // `pixels` is tightly packed 32-bit pixels in VideoOut's byte order
    // (B, G, R, A -- VK_FORMAT_B8G8R8A8_SRGB renders straight into it), one
    // row after another. `tile_ms` receives the wall time of the tile write
    // (including the cache writeback). Returns false only when VideoOut
    // refused the flip; a flip that merely had to be shown unpaced still
    // returns true and says so in `error`.
    bool present(const void* pixels, double* tile_ms, std::string& error);

    // Blank, unregister, close, unmap, release -- each step bounded. A step
    // that will not finish leaves its memory alone (VideoOut may still scan
    // it out) and is reported in `error`.
    void close(std::string* error = nullptr);

private:
    // Waits until a flip after this serial has been shown, which is when
    // VideoOut lets that flip's buffer be reused; at most pace_timeout_ms.
    // Serial 0 ("never flipped") waits for nothing.
    bool wait_shown(uint64_t serial);

    VideoOutConfig config_{};
    int handle_ = -1;
    bool registered_ = false;
    void* mapped_ = nullptr;
    int64_t physical_ = -1;
    size_t buffer_bytes_ = 0;
    size_t total_bytes_ = 0;
    int next_buffer_ = 0;
    uint64_t flip_argument_ = 0;
    uint64_t flips_ = 0;
    uint64_t buffer_serial_[8] = {};
    bool unpaced_reported_ = false;
};

}  // namespace ps5
}  // namespace splat
