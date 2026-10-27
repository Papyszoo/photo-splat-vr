// The two Vulkan helpers every title needs, in one copy: the result name for a
// log line and the memory-type search for an allocation.
//
// They were copied into platform/ps5/viewer/eye_buffers.cpp and
// platform/ps5/vr_hello/title.cpp (and a third, unused, result-name switch sat
// in title_common.h); the copies had already drifted (one named
// VK_ERROR_FORMAT_NOT_SUPPORTED, neither the other), so they live here and both
// titles include this header. It needs only Vulkan's own header: no klog, no
// pthread, nothing from the viewer — vr_hello links no viewer code but these.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace splat::ps5 {

// A VkResult as the name vulkan_core.h gave it, for a log line or an error
// string. The list is the failures a title can actually hit (device creation,
// memory, images, fences, pipeline compiles); anything else prints "VkResult"
// with the code omitted rather than a wrong name.
inline const char* vk_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        default: return "VkResult";
    }
}

// The first memory type that has every bit of `want`, preferring one that also
// has `prefer`; UINT32_MAX when none has `want`. The console's unified memory
// maps direct memory at one address for both sides, so a host-visible coherent
// type is normally DEVICE_LOCAL too and the preference costs nothing.
inline uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits,
                                 VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) == 0) continue;
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if ((flags & want) != want) continue;
        if ((flags & prefer) == prefer) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

}  // namespace splat::ps5
