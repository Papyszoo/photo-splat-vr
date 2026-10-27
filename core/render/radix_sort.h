#pragma once

#if defined(SPLAT_HAVE_VULKAN)

#include <memory>

#include "common/status.h"
#include "render/vulkan_context.h"

namespace splat {

// Reduce-then-scan 32-bit radix sort (four 8-bit passes, key-value mode),
// ported from vkgs's vendored vulkan_radix_sort. Keys sort ascending; values
// are permuted alongside. Ties are not stable across partitions.
class RadixSort {
public:
    ~RadixSort();
    RadixSort(const RadixSort&) = delete;
    RadixSort& operator=(const RadixSort&) = delete;

    // Allocates the internal histogram/scratch storage for up to
    // `max_elements` key-value pairs.
    static Result<std::unique_ptr<RadixSort>> create(VulkanContext& context,
                                                     uint32_t max_elements);

    // Records a key-value sort. When count_buffer is not VK_NULL_HANDLE the
    // element count is read from the uint32_t at `count_offset` bytes (indirect
    // sort) and must not exceed max_elements; max_elements is then only an
    // upper bound used to size the internal partition histograms.
    //
    // The ported sorter does not preserve input order for equal keys (the
    // reduce-then-scan partitions reorder ties), but it is deterministic for
    // identical input arrays, so callers that need byte-identical frames must
    // feed it a deterministic input order.
    void record(VkCommandBuffer command_buffer, VkBuffer keys, VkBuffer values,
                VkBuffer count_buffer, VkDeviceSize count_offset = 0);

    uint32_t max_elements() const { return max_elements_; }

private:
    RadixSort() = default;

    VulkanContext* context_ = nullptr;
    void* sorter_ = nullptr;  // VrdxSorter
    VulkanBuffer storage_;
    uint32_t max_elements_ = 0;
};

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
