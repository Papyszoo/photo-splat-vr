// The RADV -> Hmd2 eye-buffer handoff, shared by the viewer title's VR mode and
// vr_hello: the host-visible side-by-side eye images Hmd2 samples, and the
// one-shot image -> buffer copy that fills them on the GPU.
//
// Plain Vulkan, no core/ and no RADV loader: a title links RADV directly and
// forwards the vk* names itself (platform/ps5/viewer/vk_entrypoints.cpp), so
// this file only declares what both titles already have.
//
// Why a mapped buffer and not an image: Hmd2 samples a texture descriptor
// pointing at an address, and on this console the address vkMapMemory returns
// for a RADV host-visible buffer is the address the GPU reads (HLA-PSVR2's
// linear RGBA8 path, docs/research/hla-psvr2.md section 2). vr_hello proved it
// on hardware; this file is that code, moved so it is written once.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace splat::ps5 {

// Up to three eye images: what PS5VR and HLA-PSVR2 register (one being scanned
// out, one being written, one queued), and what hmd2's Config::buffers holds.
inline constexpr int kMaxEyeBuffers = 3;

// N host-visible, mapped, linear RGBA8 side-by-side images of width x height
// pixels (width is both eyes: 3968 on PSVR2). Hmd2 takes the mapped addresses
// as Config::external; the GPU copies the rendered frame into buffer(index)
// before each flip. Nothing is copied through the CPU.
class EyeBuffers {
public:
    EyeBuffers() = default;
    ~EyeBuffers() { destroy(); }
    EyeBuffers(const EyeBuffers&) = delete;
    EyeBuffers& operator=(const EyeBuffers&) = delete;

    // Prefers a host-visible coherent DEVICE_LOCAL type, as HLA-PSVR2 does; on
    // the console that is one unified allocation. Fails (with a message) when
    // the address is not 256-byte aligned, which Hmd2's descriptor cannot take.
    bool create(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, int count,
                uint32_t width, uint32_t height, std::string& error);
    // Unmaps and frees everything; safe to call twice, and after a failed
    // create().
    void destroy();
    // Drop the handles without touching the device: the leak-on-timeout path,
    // where a GPU wait failed and nothing may be freed under a GPU that may
    // still be reading it. The process ends with the memory held.
    void forget();


    bool ok() const { return count_ > 0; }
    int count() const { return count_; }
    uint32_t width() const { return width_; }    // both eyes
    uint32_t height() const { return height_; }
    VkDeviceSize bytes() const { return bytes_; }  // one image
    VkBuffer buffer(int index) const;
    void* mapped(int index) const;
    uint32_t memory_type(int index) const;  // for the log line

private:
    VkDevice device_ = VK_NULL_HANDLE;
    int count_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkDeviceSize bytes_ = 0;
    VkBuffer buffers_[kMaxEyeBuffers] = {};
    VkDeviceMemory memory_[kMaxEyeBuffers] = {};
    void* mapped_[kMaxEyeBuffers] = {};
    uint32_t types_[kMaxEyeBuffers] = {};
};

// Records one eye image -> eye buffer copy into an already-begun command
// buffer: the copy itself, then the buffer barrier Hmd2 needs before the flip
// (HLA-PSVR2, xr_vulkan.c:806-812). `layout` is the image's current layout and
// must be transfer-readable; the renderer leaves it TRANSFER_SRC_OPTIMAL,
// vr_hello's own eye image is GENERAL.
void record_eye_copy(VkCommandBuffer commands, VkImage image, VkImageLayout layout,
                     VkBuffer destination, uint32_t width, uint32_t height);

// One command buffer and one fence that performs a copy and waits for it with a
// timeout. The viewer uses it twice: the TV path (image -> host buffer, then
// the CPU writes VideoOut's tiles) and the VR path (image -> the eye buffer
// Hmd2 is about to scan out). Synchronous by design: at most one copy is in
// flight, which keeps the wait bounded and the buffer free of the GPU before
// the next frame reuses it.
class CopyPass {
public:
    CopyPass() = default;
    ~CopyPass() { destroy(); }
    CopyPass(const CopyPass&) = delete;
    CopyPass& operator=(const CopyPass&) = delete;

    bool create(VkDevice device, uint32_t queue_family, VkQueue queue, std::string& error);
    void destroy();
    // As EyeBuffers::forget(): leak the command pool and fence deliberately.
    void forget();

    // Record, submit and wait. False means the GPU did not finish within
    // timeout_ns (VK_TIMEOUT) or the submission failed; nothing may be freed
    // under it then and the caller must stop (leak-on-timeout).
    bool copy(VkImage image, VkImageLayout layout, VkBuffer destination, VkExtent2D extent,
              uint64_t timeout_ns, std::string& error);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

}  // namespace splat::ps5
