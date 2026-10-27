#include "render/radix_sort.h"

#if defined(SPLAT_HAVE_VULKAN)

#include <vulkan/vulkan_core.h>

// The vendored sorter spells the push-descriptor API with the Vulkan 1.4 core
// names. Vulkan headers before 1.4 (Ubuntu 24.04 ships 1.3) declare only the
// VK_KHR_push_descriptor spellings, so map the core names onto those. The
// header already resolves the entry point via "vkCmdPushDescriptorSetKHR",
// which is the same function, so behaviour does not change.
#if !defined(VK_VERSION_1_4)
using PFN_vkCmdPushDescriptorSet = PFN_vkCmdPushDescriptorSetKHR;
#define VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT \
    VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR
#endif

#define VRDX_IMPLEMENTATION
#include "render/vk_radix_sort.h"

namespace splat {

RadixSort::~RadixSort() {
    if (sorter_ != nullptr) {
        vrdxDestroySorter(static_cast<VrdxSorter>(sorter_));
        sorter_ = nullptr;
    }
    if (context_ != nullptr) {
        destroy_vulkan_buffer(*context_, storage_);
    }
}

Result<std::unique_ptr<RadixSort>> RadixSort::create(VulkanContext& context,
                                                     uint32_t max_elements) {
    if (max_elements == 0) {
        return Result<std::unique_ptr<RadixSort>>::failure(
            "radix sort needs a non-zero element count");
    }

    std::unique_ptr<RadixSort> sort(new RadixSort());
    sort->context_ = &context;
    sort->max_elements_ = max_elements;

    VrdxSorterCreateInfo info{};
    info.physicalDevice = context.physical_device();
    info.device = context.device();
    info.pipelineCache = VK_NULL_HANDLE;
    VrdxSorter sorter = VK_NULL_HANDLE;
    const VkResult result = vrdxCreateSorter(&info, &sorter);
    if (result != VK_SUCCESS) {
        return Result<std::unique_ptr<RadixSort>>::failure(
            "vrdxCreateSorter failed (VkResult " + std::to_string(static_cast<int>(result)) +
            ")");
    }
    sort->sorter_ = sorter;

    VrdxSorterStorageRequirements requirements{};
    vrdxGetSorterStorageRequirements(sorter, max_elements, VRDX_SORT_MODE_KEY_VALUE,
                                     &requirements);
    auto storage = create_vulkan_buffer(context, requirements.size, requirements.usage);
    if (!storage.ok()) {
        return Result<std::unique_ptr<RadixSort>>::failure(storage.message());
    }
    sort->storage_ = storage.take();
    return Result<std::unique_ptr<RadixSort>>::success(std::move(sort));
}

void RadixSort::record(VkCommandBuffer command_buffer, VkBuffer keys, VkBuffer values,
                       VkBuffer count_buffer, VkDeviceSize count_offset) {
    VrdxSortInfo info{};
    info.elementCount = max_elements_;
    info.elementCountBuffer = count_buffer;
    info.elementCountOffset = count_offset;
    info.keysBuffer = keys;
    info.keysOffset = 0;
    info.valuesBuffer = values;
    info.valuesOffset = 0;
    info.storageBuffer = storage_.buffer;
    info.storageOffset = 0;
    info.queryPool = VK_NULL_HANDLE;
    info.query = 0;
    vrdxCmdSort(command_buffer, static_cast<VrdxSorter>(sorter_), &info);
}

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
