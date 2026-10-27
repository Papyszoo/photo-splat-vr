// The eye buffers and the copy pass declared in eye_buffers.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eye_buffers.h"

#include <cstdio>
#include <cstring>

#include "vk_common.h"

namespace splat::ps5 {

bool EyeBuffers::create(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, int count,
                        uint32_t width, uint32_t height, std::string& error) {
    destroy();
    if (device == VK_NULL_HANDLE || width == 0 || height == 0) {
        error = "eye buffers need a device and a non-zero size";
        return false;
    }
    if (count < 1 || count > kMaxEyeBuffers) {
        error = "eye buffer count must be 1..3";
        return false;
    }
    device_ = device;
    width_ = width;
    height_ = height;
    bytes_ = static_cast<VkDeviceSize>(width) * height * 4u;
    for (int i = 0; i < count; i++) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes_;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &info, nullptr, &buffers_[i]);
        if (result != VK_SUCCESS) {
            error = std::string("vkCreateBuffer -> ") + vk_name(result);
            destroy();
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffers_[i], &requirements);
        const uint32_t type = find_memory_type(
            memory, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            error = "no host-visible coherent memory type for an eye buffer";
            destroy();
            return false;
        }
        types_[i] = type;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        result = vkAllocateMemory(device, &allocate, nullptr, &memory_[i]);
        if (result != VK_SUCCESS) {
            error = std::string("vkAllocateMemory -> ") + vk_name(result);
            destroy();
            return false;
        }
        result = vkBindBufferMemory(device, buffers_[i], memory_[i], 0);
        if (result != VK_SUCCESS) {
            error = std::string("vkBindBufferMemory -> ") + vk_name(result);
            destroy();
            return false;
        }
        result = vkMapMemory(device, memory_[i], 0, VK_WHOLE_SIZE, 0, &mapped_[i]);
        if (result != VK_SUCCESS) {
            error = std::string("vkMapMemory -> ") + vk_name(result);
            destroy();
            return false;
        }
        // Hmd2's linear descriptor carries the address in 256-byte units.
        if ((reinterpret_cast<uintptr_t>(mapped_[i]) & 0xffu) != 0) {
            error = "the mapped eye buffer is not 256-byte aligned";
            destroy();
            return false;
        }
        std::memset(mapped_[i], 0, static_cast<size_t>(bytes_));
        count_ = i + 1;
    }
    return true;
}

void EyeBuffers::destroy() {
    if (device_ == VK_NULL_HANDLE) return;
    for (int i = 0; i < kMaxEyeBuffers; i++) {
        if (mapped_[i] != nullptr && memory_[i] != VK_NULL_HANDLE)
            vkUnmapMemory(device_, memory_[i]);
        if (buffers_[i] != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffers_[i], nullptr);
        if (memory_[i] != VK_NULL_HANDLE) vkFreeMemory(device_, memory_[i], nullptr);
        buffers_[i] = VK_NULL_HANDLE;
        memory_[i] = VK_NULL_HANDLE;
        mapped_[i] = nullptr;
        types_[i] = 0;
    }
    device_ = VK_NULL_HANDLE;
    count_ = 0;
    width_ = height_ = 0;
    bytes_ = 0;
}

void EyeBuffers::forget() {
    for (int i = 0; i < kMaxEyeBuffers; i++) {
        buffers_[i] = VK_NULL_HANDLE;
        memory_[i] = VK_NULL_HANDLE;
        mapped_[i] = nullptr;
        types_[i] = 0;
    }
    device_ = VK_NULL_HANDLE;
    count_ = 0;
    width_ = height_ = 0;
    bytes_ = 0;
}

VkBuffer EyeBuffers::buffer(int index) const {
    return index >= 0 && index < count_ ? buffers_[index] : VK_NULL_HANDLE;
}

void* EyeBuffers::mapped(int index) const {
    return index >= 0 && index < count_ ? mapped_[index] : nullptr;
}

uint32_t EyeBuffers::memory_type(int index) const {
    return index >= 0 && index < count_ ? types_[index] : 0;
}

void record_eye_copy(VkCommandBuffer commands, VkImage image, VkImageLayout layout,
                     VkBuffer destination, uint32_t width, uint32_t height) {
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(commands, image, layout, destination, 1, &region);

    // The GPU wrote the buffer; make it visible to Hmd2 before the flip
    // (HLA-PSVR2's barrier, xr_vulkan.c:806-812).
    VkBufferMemoryBarrier to_host{};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = destination;
    to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 1, &to_host, 0, nullptr);
}

bool CopyPass::create(VkDevice device, uint32_t queue_family, VkQueue queue, std::string& error) {
    destroy();
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
        error = "copy pass needs a device and a queue";
        return false;
    }
    device_ = device;
    queue_ = queue;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &pool_);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateCommandPool -> ") + vk_name(result);
        destroy();
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate, &commands_);
    if (result != VK_SUCCESS) {
        error = std::string("vkAllocateCommandBuffers -> ") + vk_name(result);
        destroy();
        return false;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device, &fence_info, nullptr, &fence_);
    if (result != VK_SUCCESS) {
        error = std::string("vkCreateFence -> ") + vk_name(result);
        destroy();
        return false;
    }
    return true;
}

void CopyPass::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, pool_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    commands_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
}

void CopyPass::forget() {
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    commands_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
}

bool CopyPass::copy(VkImage image, VkImageLayout layout, VkBuffer destination, VkExtent2D extent,
                    uint64_t timeout_ns, std::string& error) {
    if (device_ == VK_NULL_HANDLE) {
        error = "the copy pass was not created";
        return false;
    }
    VkResult result = vkResetCommandBuffer(commands_, 0);
    if (result != VK_SUCCESS) {
        error = std::string("vkResetCommandBuffer -> ") + vk_name(result);
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commands_, &begin);
    if (result != VK_SUCCESS) {
        error = std::string("vkBeginCommandBuffer -> ") + vk_name(result);
        return false;
    }
    record_eye_copy(commands_, image, layout, destination, extent.width, extent.height);
    result = vkEndCommandBuffer(commands_);
    if (result != VK_SUCCESS) {
        error = std::string("vkEndCommandBuffer -> ") + vk_name(result);
        return false;
    }
    vkResetFences(device_, 1, &fence_);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    result = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (result != VK_SUCCESS) {
        error = std::string("vkQueueSubmit -> ") + vk_name(result);
        return false;
    }
    result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
    if (result != VK_SUCCESS) {
        char text[160];
        std::snprintf(text, sizeof(text), "vkWaitForFences -> %s (%llu ms)", vk_name(result),
                      static_cast<unsigned long long>(timeout_ns / 1000000ull));
        error = text;
        return false;
    }
    return true;
}

}  // namespace splat::ps5
