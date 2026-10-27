// Loader-less Vulkan entry points for the PS5 viewer title.
//
// The C6 renderer (core/render) calls Vulkan by name: vkCreateInstance,
// vkCmdCopyImageToBuffer, vkWaitForFences, ... That is what a program with a
// Vulkan loader does, and it is also how the desktop build works. A PS5 title
// links RADV's archive directly and has no loader: libvulkan_radeon.ps5.a
// exports exactly one symbol, vk_icdGetInstanceProcAddr, and nothing else (checked
// with llvm-nm on the archive this project builds). Linking the renderer as it
// is therefore needs one forwarding definition per entry point it names.
//
// Each shim resolves its pointer once through the same two lookups a loader
// makes -- vkGetDeviceProcAddr for a device command, vk_icdGetInstanceProcAddr
// otherwise -- and then calls it. There is no dispatch table of our own, no
// layer, no allocation, and no behaviour change: the renderer cannot tell the
// difference. platform/ps5/ggml/ps5_vulkan.cpp does the same for the five
// entry points ggml-vulkan names.
//
// vkGetDeviceProcAddr, vkGetPhysicalDeviceFeatures2 and vkCmdCopyBuffer are
// deliberately NOT defined here: ps5_vulkan.cpp (the glue archive linked into
// this title with SHARP) defines exactly those three for ggml-vulkan, and one
// definition per symbol is enough for both users. The glue is also the only
// source of vkGetInstanceProcAddr, which vulkan-hpp asks for by name. They
// resolve their pointers through the instance they captured from a
// vkGetInstanceProcAddr call, and Mesa's vk_icdGetInstanceProcAddr returns NULL
// for every one of them when it is given a null instance, so vkCreateInstance
// below hands them the instance as soon as RADV makes it (the renderer creates
// the instance before ggml-vulkan does).
//
// The list is exactly the vk* names in core/render/*.{h,cpp}; adding a Vulkan
// call to the renderer means adding its shim here (the link fails loudly,
// naming the symbol, until it is added). The shims stay local to the title
// (the Makefile puts them in the version script), so nothing is exported.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>

#include <vulkan/vulkan.h>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char * name);

// Defined in platform/ps5/ggml/ps5_vulkan.cpp (the glue archive). Its shims
// resolve through the instance it captures from this call; see the header note.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char * name);

namespace {

VkInstance g_instance = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
PFN_vkGetDeviceProcAddr g_get_device_proc_addr = nullptr;

// One lookup, as a loader does it: a device command through the device, the
// rest through the instance, and the few instance-independent ones (and any
// early call before vkCreateInstance) through a null instance.
template <typename Fn>
Fn resolve(const char * name) {
    if (g_device != VK_NULL_HANDLE) {
        if (g_get_device_proc_addr == nullptr) {
            g_get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                vk_icdGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr"));
        }
        if (g_get_device_proc_addr != nullptr) {
            PFN_vkVoidFunction found = g_get_device_proc_addr(g_device, name);
            if (found != nullptr) return reinterpret_cast<Fn>(found);
        }
    }
    PFN_vkVoidFunction found = vk_icdGetInstanceProcAddr(g_instance, name);
    if (found != nullptr) return reinterpret_cast<Fn>(found);
    return reinterpret_cast<Fn>(vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, name));
}

}  // namespace

// The three that keep the state the others resolve through.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo * pCreateInfo,
                 const VkAllocationCallbacks * pAllocator, VkInstance * pInstance) {
    static PFN_vkCreateInstance fn = nullptr;
    if (fn == nullptr) {
        fn = reinterpret_cast<PFN_vkCreateInstance>(
            vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    }
    if (fn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = fn(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS && pInstance != nullptr) {
        g_instance = *pInstance;
        // Seed ps5_vulkan.cpp's captured instance: it resolves the four shims
        // this file does not define through vk_icdGetInstanceProcAddr, which
        // answers NULL without an instance, and the renderer asks for
        // vkGetPhysicalDeviceFeatures2 and vkCmdCopyBuffer before ggml-vulkan
        // makes its own vkGetInstanceProcAddr call.
        (void)vkGetInstanceProcAddr(*pInstance, "vkGetInstanceProcAddr");
    }
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo,
               const VkAllocationCallbacks * pAllocator, VkDevice * pDevice) {
    static PFN_vkCreateDevice fn = nullptr;
    if (fn == nullptr) fn = resolve<PFN_vkCreateDevice>("vkCreateDevice");
    if (fn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS && pDevice != nullptr) {
        g_device = *pDevice;
        g_get_device_proc_addr = nullptr;  // a new device may answer differently
    }
    return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks * pAllocator) {
    static PFN_vkDestroyDevice fn = nullptr;
    if (fn == nullptr) fn = resolve<PFN_vkDestroyDevice>("vkDestroyDevice");
    if (fn != nullptr) fn(device, pAllocator);
    if (device == g_device) {
        g_device = VK_NULL_HANDLE;
        g_get_device_proc_addr = nullptr;
    }
}

// vkGetDeviceProcAddr, vkGetPhysicalDeviceFeatures2 and vkCmdCopyBuffer are not
// defined here: ps5_vulkan.cpp (the glue archive) defines them for ggml-vulkan
// and the renderer shares them. See the note at the top.

// Everything else: one forwarding definition each, generated from the Vulkan
// header's own prototypes so the signature cannot drift from it.
#define SHIM_RET(return_type, function, parameters, arguments)                     \
    extern "C" VKAPI_ATTR return_type VKAPI_CALL function parameters {             \
        static PFN_##function fn = nullptr;                                        \
        if (fn == nullptr) fn = resolve<PFN_##function>(#function);                \
        if (fn == nullptr) return VK_ERROR_INITIALIZATION_FAILED;                  \
        return fn arguments;                                                       \
    }
#define SHIM_VOID(function, parameters, arguments)                                 \
    extern "C" VKAPI_ATTR void VKAPI_CALL function parameters {                    \
        static PFN_##function fn = nullptr;                                        \
        if (fn == nullptr) fn = resolve<PFN_##function>(#function);                \
        if (fn != nullptr) fn arguments;                                           \
    }

SHIM_RET(VkResult, vkAllocateCommandBuffers, (VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers), (device, pAllocateInfo, pCommandBuffers))
SHIM_RET(VkResult, vkAllocateDescriptorSets, (VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets), (device, pAllocateInfo, pDescriptorSets))
SHIM_RET(VkResult, vkAllocateMemory, (VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory), (device, pAllocateInfo, pAllocator, pMemory))
SHIM_RET(VkResult, vkBeginCommandBuffer, (VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo), (commandBuffer, pBeginInfo))
SHIM_RET(VkResult, vkBindBufferMemory, (VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset), (device, buffer, memory, memoryOffset))
SHIM_RET(VkResult, vkBindImageMemory, (VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset), (device, image, memory, memoryOffset))
SHIM_VOID(vkCmdBeginRendering, (VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo), (commandBuffer, pRenderingInfo))
SHIM_VOID(vkCmdBlitImage, (VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter), (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter))
SHIM_VOID(vkCmdBindDescriptorSets, (VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets), (commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets))
SHIM_VOID(vkCmdBindPipeline, (VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline), (commandBuffer, pipelineBindPoint, pipeline))
SHIM_VOID(vkCmdBindVertexBuffers, (VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets), (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets))
SHIM_VOID(vkCmdCopyBufferToImage, (VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions), (commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions))
SHIM_VOID(vkCmdCopyImageToBuffer, (VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions), (commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions))
SHIM_VOID(vkCmdDispatch, (VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ), (commandBuffer, groupCountX, groupCountY, groupCountZ))
SHIM_VOID(vkCmdDraw, (VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance), (commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance))
SHIM_VOID(vkCmdDrawIndirect, (VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride), (commandBuffer, buffer, offset, drawCount, stride))
SHIM_VOID(vkCmdEndRendering, (VkCommandBuffer commandBuffer), (commandBuffer))
SHIM_VOID(vkCmdFillBuffer, (VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data), (commandBuffer, dstBuffer, dstOffset, size, data))
SHIM_VOID(vkCmdPipelineBarrier, (VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers), (commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers))
SHIM_VOID(vkCmdPipelineBarrier2, (VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo), (commandBuffer, pDependencyInfo))
SHIM_VOID(vkCmdPushConstants, (VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues), (commandBuffer, layout, stageFlags, offset, size, pValues))
SHIM_VOID(vkCmdPushDescriptorSetKHR, (VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites), (commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites))
SHIM_VOID(vkCmdResetQueryPool, (VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount), (commandBuffer, queryPool, firstQuery, queryCount))
SHIM_VOID(vkCmdSetScissor, (VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors), (commandBuffer, firstScissor, scissorCount, pScissors))
SHIM_VOID(vkCmdSetViewport, (VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports), (commandBuffer, firstViewport, viewportCount, pViewports))
SHIM_VOID(vkCmdUpdateBuffer, (VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void* pData), (commandBuffer, dstBuffer, dstOffset, dataSize, pData))
SHIM_VOID(vkCmdWriteTimestamp, (VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t query), (commandBuffer, pipelineStage, queryPool, query))
SHIM_RET(VkResult, vkCreateBuffer, (VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer), (device, pCreateInfo, pAllocator, pBuffer))
SHIM_RET(VkResult, vkCreateCommandPool, (VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool), (device, pCreateInfo, pAllocator, pCommandPool))
SHIM_RET(VkResult, vkCreateComputePipelines, (VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines), (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines))
SHIM_RET(VkResult, vkCreateDescriptorPool, (VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool), (device, pCreateInfo, pAllocator, pDescriptorPool))
SHIM_RET(VkResult, vkCreateDescriptorSetLayout, (VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout), (device, pCreateInfo, pAllocator, pSetLayout))
SHIM_RET(VkResult, vkCreateFence, (VkDevice device, const VkFenceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence), (device, pCreateInfo, pAllocator, pFence))
SHIM_RET(VkResult, vkCreateGraphicsPipelines, (VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines), (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines))
SHIM_RET(VkResult, vkCreateImage, (VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage), (device, pCreateInfo, pAllocator, pImage))
SHIM_RET(VkResult, vkCreateImageView, (VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView), (device, pCreateInfo, pAllocator, pView))
SHIM_RET(VkResult, vkCreatePipelineLayout, (VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout), (device, pCreateInfo, pAllocator, pPipelineLayout))
SHIM_RET(VkResult, vkCreateQueryPool, (VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkQueryPool* pQueryPool), (device, pCreateInfo, pAllocator, pQueryPool))
SHIM_RET(VkResult, vkCreateSampler, (VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler), (device, pCreateInfo, pAllocator, pSampler))
SHIM_RET(VkResult, vkCreateShaderModule, (VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule), (device, pCreateInfo, pAllocator, pShaderModule))
SHIM_VOID(vkDestroyBuffer, (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator), (device, buffer, pAllocator))
SHIM_VOID(vkDestroyCommandPool, (VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator), (device, commandPool, pAllocator))
SHIM_VOID(vkDestroyDescriptorPool, (VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks* pAllocator), (device, descriptorPool, pAllocator))
SHIM_VOID(vkDestroyDescriptorSetLayout, (VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks* pAllocator), (device, descriptorSetLayout, pAllocator))
SHIM_VOID(vkDestroyFence, (VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator), (device, fence, pAllocator))
SHIM_VOID(vkDestroyImage, (VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator), (device, image, pAllocator))
SHIM_VOID(vkDestroyImageView, (VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator), (device, imageView, pAllocator))
SHIM_VOID(vkDestroyInstance, (VkInstance instance, const VkAllocationCallbacks* pAllocator), (instance, pAllocator))
SHIM_VOID(vkDestroyPipeline, (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator), (device, pipeline, pAllocator))
SHIM_VOID(vkDestroyPipelineLayout, (VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks* pAllocator), (device, pipelineLayout, pAllocator))
SHIM_VOID(vkDestroyQueryPool, (VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks* pAllocator), (device, queryPool, pAllocator))
SHIM_VOID(vkDestroySampler, (VkDevice device, VkSampler sampler, const VkAllocationCallbacks* pAllocator), (device, sampler, pAllocator))
SHIM_VOID(vkDestroyShaderModule, (VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator), (device, shaderModule, pAllocator))
SHIM_RET(VkResult, vkDeviceWaitIdle, (VkDevice device), (device))
SHIM_RET(VkResult, vkEndCommandBuffer, (VkCommandBuffer commandBuffer), (commandBuffer))
SHIM_RET(VkResult, vkEnumerateDeviceExtensionProperties, (VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties), (physicalDevice, pLayerName, pPropertyCount, pProperties))
SHIM_RET(VkResult, vkEnumerateInstanceExtensionProperties, (const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties), (pLayerName, pPropertyCount, pProperties))
SHIM_RET(VkResult, vkEnumeratePhysicalDevices, (VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices), (instance, pPhysicalDeviceCount, pPhysicalDevices))
SHIM_VOID(vkFreeCommandBuffers, (VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers), (device, commandPool, commandBufferCount, pCommandBuffers))
SHIM_RET(VkResult, vkFreeDescriptorSets, (VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets), (device, descriptorPool, descriptorSetCount, pDescriptorSets))
SHIM_VOID(vkFreeMemory, (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator), (device, memory, pAllocator))
SHIM_VOID(vkGetBufferMemoryRequirements, (VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements), (device, buffer, pMemoryRequirements))
SHIM_VOID(vkGetDeviceQueue, (VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue), (device, queueFamilyIndex, queueIndex, pQueue))
SHIM_VOID(vkGetImageMemoryRequirements, (VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements), (device, image, pMemoryRequirements))
SHIM_VOID(vkGetPhysicalDeviceMemoryProperties, (VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties), (physicalDevice, pMemoryProperties))
SHIM_VOID(vkGetPhysicalDeviceFeatures, (VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures), (physicalDevice, pFeatures))
SHIM_VOID(vkGetPhysicalDeviceProperties, (VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties), (physicalDevice, pProperties))
SHIM_VOID(vkGetPhysicalDeviceProperties2, (VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties), (physicalDevice, pProperties))
SHIM_VOID(vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties), (physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties))
SHIM_RET(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR, (VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported), (physicalDevice, queueFamilyIndex, surface, pSupported))
SHIM_RET(VkResult, vkGetQueryPoolResults, (VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags), (device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags))
SHIM_RET(VkResult, vkMapMemory, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData), (device, memory, offset, size, flags, ppData))
SHIM_RET(VkResult, vkQueueSubmit, (VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence), (queue, submitCount, pSubmits, fence))
SHIM_RET(VkResult, vkQueueWaitIdle, (VkQueue queue), (queue))
SHIM_RET(VkResult, vkResetCommandBuffer, (VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags), (commandBuffer, flags))
SHIM_RET(VkResult, vkResetFences, (VkDevice device, uint32_t fenceCount, const VkFence* pFences), (device, fenceCount, pFences))
SHIM_VOID(vkUnmapMemory, (VkDevice device, VkDeviceMemory memory), (device, memory))
SHIM_VOID(vkUpdateDescriptorSets, (VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies), (device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies))
SHIM_RET(VkResult, vkWaitForFences, (VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout), (device, fenceCount, pFences, waitAll, timeout))

#undef SHIM_RET
#undef SHIM_VOID
