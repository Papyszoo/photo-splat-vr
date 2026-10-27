#pragma once

#include <memory>

#if defined(SPLAT_HAVE_VULKAN)

#include <cstdint>

#include <vulkan/vulkan.h>

#include "common/status.h"

namespace splat {

// Options for create_vulkan_context. When `instance` is VK_NULL_HANDLE the
// context creates its own headless instance (portability enumeration included
// on macOS). A provided instance belongs to the caller and is not destroyed.
// `surface` is only used for device selection when `present` is true.
struct VulkanContextOptions {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool present = false;
};

// A Vulkan 1.3 device with a graphics+compute queue, the features the splat
// renderer needs (dynamic rendering, synchronization2, 16-bit storage,
// shaderFloat16) and VK_KHR_push_descriptor for the radix sort.
class VulkanContext {
public:
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    static Result<std::unique_ptr<VulkanContext>> create(const VulkanContextOptions& options);

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    VkQueue present_queue() const { return present_queue_; }
    uint32_t queue_family() const { return queue_family_; }
    uint32_t present_family() const { return present_family_; }
    VkCommandPool command_pool() const { return command_pool_; }
    bool owns_instance() const { return owns_instance_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    const VkPhysicalDeviceMemoryProperties& memory_properties() const { return memory_; }

    // Nanoseconds per timestamp tick, 0 when the device reports no timestamps.
    float timestamp_period() const { return timestamp_period_; }
    bool timestamps_supported() const { return timestamps_supported_; }

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) const;

    // Whether the device can filter with anisotropy (VR9's overlay mip
    // sampling). Queried on the feature struct the device is created with; the
    // PS5's RADV and desktop Vulkan both report it.
    bool sampler_anisotropy() const { return sampler_anisotropy_; }
    float max_sampler_anisotropy() const { return max_sampler_anisotropy_; }

private:
    VulkanContext() = default;
    void destroy();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    uint32_t present_family_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    bool owns_instance_ = false;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    float timestamp_period_ = 0.0f;
    bool timestamps_supported_ = false;
    bool sampler_anisotropy_ = false;
    float max_sampler_anisotropy_ = 1.0f;
};

// Host-visible, host-coherent buffer that stays mapped. The desktop renderer
// and the PS5 both have unified memory; a discrete-GPU staging path is a
// console/backlog item.
struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

Result<VulkanBuffer> create_vulkan_buffer(VulkanContext& context, VkDeviceSize size,
                                          VkBufferUsageFlags usage,
                                          const void* data = nullptr,
                                          VkMemoryPropertyFlags properties =
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
void destroy_vulkan_buffer(VulkanContext& context, VulkanBuffer& buffer);

// Color image plus view, used for offscreen rendering and readback.
struct VulkanImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t mip_levels = 1;  // the view covers levels 0 .. mip_levels-1
};

// The full mip chain length for an extent: floor(log2(max(w, h))) + 1. VR9's
// overlay textures use it so the sampler can pick a filtered level instead of
// minifying level 0 (the menu panel and the scene label flicker).
uint32_t mip_level_count(uint32_t width, uint32_t height);

Result<VulkanImage> create_vulkan_color_image(
    VulkanContext& context, VkFormat format, VkExtent2D extent,
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, uint32_t mip_levels = 1);
// D32_SFLOAT depth attachment the renderer samples in the comfort pass to know
// how far the nearest splat at each pixel is.
Result<VulkanImage> create_vulkan_depth_image(VulkanContext& context, VkExtent2D extent);
void destroy_vulkan_image(VulkanContext& context, VulkanImage& image);

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
