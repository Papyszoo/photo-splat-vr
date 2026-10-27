#include "render/vulkan_context.h"

#if defined(SPLAT_HAVE_VULKAN)

#include <algorithm>
#include <cstring>
#include <vector>

namespace splat {
namespace {

std::string vk_error(const char* what, VkResult result) {
    return std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) +
           ")";
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    return extensions;
}

bool create_instance(VulkanContextOptions const& options, VkInstance& instance,
                     std::string& error) {
    if (options.instance != VK_NULL_HANDLE) {
        instance = options.instance;
        return true;
    }

    uint32_t available_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
    std::vector<VkExtensionProperties> available(available_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data());

    // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR is only legal when the
    // extension is also in the enabled list (VUID-VkInstanceCreateInfo-flags-
    // 06776), so one flag drives both.
    bool portability = false;
#if defined(__APPLE__)
    portability = has_extension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
    std::vector<const char*> extensions;
    if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "splat";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "ps5-splat-vr";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    if (portability) info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    const VkResult result = vkCreateInstance(&info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        error = vk_error("vkCreateInstance", result);
        return false;
    }
    return true;
}

struct DeviceChoice {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    uint32_t present_family = 0;
    VkPhysicalDeviceProperties properties{};
    bool sampler_anisotropy = false;
};

bool device_supported(VkPhysicalDevice device, VkSurfaceKHR surface, bool present,
                      DeviceChoice& choice, std::string& reason) {
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());

    uint32_t graphics = UINT32_MAX;
    uint32_t present_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (graphics == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            graphics = i;
        }
        if (present && surface != VK_NULL_HANDLE) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported);
            if (supported && present_family == UINT32_MAX) present_family = i;
        }
    }
    if (graphics == UINT32_MAX) {
        reason = "no graphics+compute queue family";
        return false;
    }
    if (present && present_family == UINT32_MAX) {
        reason = "no queue family can present to the surface";
        return false;
    }

    const std::vector<VkExtensionProperties> extensions = device_extensions(device);
    if (!has_extension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        reason = "VK_KHR_push_descriptor is required by the radix sort";
        return false;
    }
    if (present && !has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        reason = "no VK_KHR_swapchain";
        return false;
    }

    VkPhysicalDeviceVulkan11Features features11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features float16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.pNext = &float16;
    float16.pNext = &features11;

    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &features13;
    vkGetPhysicalDeviceFeatures2(device, &query);

    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(device, &properties2);

    if (!features13.synchronization2) {
        reason = "synchronization2 is required";
        return false;
    }
    if (!features13.dynamicRendering) {
        reason = "dynamic rendering is required";
        return false;
    }
    if (!features11.storageBuffer16BitAccess) {
        reason = "16-bit storage buffers are required";
        return false;
    }
    if (!float16.shaderFloat16) {
        reason = "shaderFloat16 is required";
        return false;
    }
    if (subgroup.subgroupSize != 32 && subgroup.subgroupSize != 64) {
        reason = "subgroup size must be 32 or 64";
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        reason = "Vulkan 1.3 is required";
        return false;
    }

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device, &features);

    choice.device = device;
    choice.family = graphics;
    choice.present_family = present ? present_family : graphics;
    choice.properties = properties;
    // Anisotropic filtering is optional (VR9's overlay mips use it when it is
    // there, plain trilinear otherwise).
    choice.sampler_anisotropy = features.samplerAnisotropy == VK_TRUE;
    return true;
}

}  // namespace

VulkanContext::~VulkanContext() { destroy(); }

void VulkanContext::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (command_pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, command_pool_, nullptr);
            command_pool_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (owns_instance_ && instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
}

uint32_t VulkanContext::find_memory_type(uint32_t type_bits,
                                         VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (memory_.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

Result<std::unique_ptr<VulkanContext>> VulkanContext::create(
    const VulkanContextOptions& options) {
    std::unique_ptr<VulkanContext> context(new VulkanContext());
    std::string error;

    if (!create_instance(options, context->instance_, error)) {
        return Result<std::unique_ptr<VulkanContext>>::failure(error);
    }
    context->owns_instance_ = options.instance == VK_NULL_HANDLE;

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(context->instance_, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        return Result<std::unique_ptr<VulkanContext>>::failure(
            "no Vulkan physical devices (is an ICD installed?)");
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(context->instance_, &count, devices.data());

    DeviceChoice choice;
    std::string reasons;
    for (VkPhysicalDevice device : devices) {
        std::string reason;
        if (device_supported(device, options.surface, options.present, choice, reason)) break;
        reasons += (reasons.empty() ? "" : "; ") + reason;
    }
    if (choice.device == VK_NULL_HANDLE) {
        return Result<std::unique_ptr<VulkanContext>>::failure(
            "no usable Vulkan device: " + (reasons.empty() ? std::string("none") : reasons));
    }

    std::vector<VkDeviceQueueCreateInfo> queues;
    std::vector<uint32_t> unique = {choice.family};
    if (choice.present_family != choice.family) unique.push_back(choice.present_family);
    const float priority = 1.0f;
    for (uint32_t family : unique) {
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        queues.push_back(queue);
    }

    const std::vector<VkExtensionProperties> extensions = device_extensions(choice.device);
    std::vector<const char*> device_extensions_list = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (options.present) device_extensions_list.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (has_extension(extensions, "VK_KHR_portability_subset")) {
        device_extensions_list.push_back("VK_KHR_portability_subset");
    }

    VkPhysicalDeviceVulkan11Features features11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.storageBuffer16BitAccess = VK_TRUE;
    features11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceShaderFloat16Int8Features float16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    float16.shaderFloat16 = VK_TRUE;
    float16.pNext = &features11;
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.synchronization2 = VK_TRUE;
    features13.dynamicRendering = VK_TRUE;
    features13.pNext = &float16;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &features13;
    // Enabled core features: anisotropy for the overlay sampler when the device
    // has it (queried above); nothing else is requested here.
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.samplerAnisotropy = choice.sampler_anisotropy ? VK_TRUE : VK_FALSE;
    info.pEnabledFeatures = &enabled_features;
    info.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
    info.pQueueCreateInfos = queues.data();
    info.enabledExtensionCount = static_cast<uint32_t>(device_extensions_list.size());
    info.ppEnabledExtensionNames = device_extensions_list.data();

    VkResult result = vkCreateDevice(choice.device, &info, nullptr, &context->device_);
    if (result != VK_SUCCESS) {
        return Result<std::unique_ptr<VulkanContext>>::failure(
            vk_error("vkCreateDevice", result));
    }

    context->physical_ = choice.device;
    context->queue_family_ = choice.family;
    context->present_family_ = choice.present_family;
    context->properties_ = choice.properties;
    context->sampler_anisotropy_ = choice.sampler_anisotropy;
    context->max_sampler_anisotropy_ =
        choice.sampler_anisotropy ? std::max(1.0f, choice.properties.limits.maxSamplerAnisotropy)
                                  : 1.0f;
    vkGetPhysicalDeviceMemoryProperties(choice.device, &context->memory_);
    vkGetDeviceQueue(context->device_, choice.family, 0, &context->queue_);
    vkGetDeviceQueue(context->device_, choice.present_family, 0, &context->present_queue_);

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = choice.family;
    result = vkCreateCommandPool(context->device_, &pool, nullptr, &context->command_pool_);
    if (result != VK_SUCCESS) {
        return Result<std::unique_ptr<VulkanContext>>::failure(
            vk_error("vkCreateCommandPool", result));
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(choice.device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(choice.device, &family_count, families.data());
    const bool timestamp_bits =
        choice.family < families.size() && families[choice.family].timestampValidBits > 0;
    context->timestamps_supported_ = timestamp_bits &&
                                     context->properties_.limits.timestampComputeAndGraphics ==
                                         VK_TRUE &&
                                     context->properties_.limits.timestampPeriod > 0.0f;
    context->timestamp_period_ = context->timestamps_supported_
                                     ? context->properties_.limits.timestampPeriod
                                     : 0.0f;

    return Result<std::unique_ptr<VulkanContext>>::success(std::move(context));
}

Result<VulkanBuffer> create_vulkan_buffer(VulkanContext& context, VkDeviceSize size,
                                          VkBufferUsageFlags usage, const void* data,
                                          VkMemoryPropertyFlags properties) {
    VulkanBuffer buffer;
    buffer.size = std::max<VkDeviceSize>(size, 16);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = buffer.size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(context.device(), &info, nullptr, &buffer.buffer);
    if (result != VK_SUCCESS) {
        return Result<VulkanBuffer>::failure(vk_error("vkCreateBuffer", result));
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device(), buffer.buffer, &requirements);
    const uint32_t type = context.find_memory_type(requirements.memoryTypeBits, properties);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
        return Result<VulkanBuffer>::failure("no suitable memory type for a buffer");
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = vkAllocateMemory(context.device(), &allocate, nullptr, &buffer.memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
        return Result<VulkanBuffer>::failure(vk_error("vkAllocateMemory", result));
    }
    result = vkBindBufferMemory(context.device(), buffer.buffer, buffer.memory, 0);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
        vkFreeMemory(context.device(), buffer.memory, nullptr);
        return Result<VulkanBuffer>::failure(vk_error("vkBindBufferMemory", result));
    }

    if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        result = vkMapMemory(context.device(), buffer.memory, 0, VK_WHOLE_SIZE, 0,
                             &buffer.mapped);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
            vkFreeMemory(context.device(), buffer.memory, nullptr);
            return Result<VulkanBuffer>::failure(vk_error("vkMapMemory", result));
        }
        if (data != nullptr) {
            std::memcpy(buffer.mapped, data, static_cast<size_t>(size));
        }
    }
    return Result<VulkanBuffer>::success(buffer);
}

void destroy_vulkan_buffer(VulkanContext& context, VulkanBuffer& buffer) {
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(context.device(), buffer.memory);
        buffer.mapped = nullptr;
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(context.device(), buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.device(), buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

uint32_t mip_level_count(uint32_t width, uint32_t height) {
    uint32_t size = std::max(width, height);
    uint32_t levels = 1;
    while (size > 1) {
        size >>= 1;
        ++levels;
    }
    return levels;
}

Result<VulkanImage> create_vulkan_color_image(VulkanContext& context, VkFormat format,
                                              VkExtent2D extent, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect,
                                              uint32_t mip_levels) {
    VulkanImage image;
    image.format = format;
    image.extent = extent;
    image.mip_levels = std::max(1u, mip_levels);

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = image.mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(context.device(), &info, nullptr, &image.image);
    if (result != VK_SUCCESS) {
        return Result<VulkanImage>::failure(vk_error("vkCreateImage", result));
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context.device(), image.image, &requirements);
    const uint32_t type = context.find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        vkDestroyImage(context.device(), image.image, nullptr);
        return Result<VulkanImage>::failure("no device-local memory type for an image");
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = vkAllocateMemory(context.device(), &allocate, nullptr, &image.memory);
    if (result != VK_SUCCESS) {
        vkDestroyImage(context.device(), image.image, nullptr);
        return Result<VulkanImage>::failure(vk_error("vkAllocateMemory", result));
    }
    result = vkBindImageMemory(context.device(), image.image, image.memory, 0);
    if (result != VK_SUCCESS) {
        vkDestroyImage(context.device(), image.image, nullptr);
        vkFreeMemory(context.device(), image.memory, nullptr);
        return Result<VulkanImage>::failure(vk_error("vkBindImageMemory", result));
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = image.mip_levels;
    view.subresourceRange.layerCount = 1;
    result = vkCreateImageView(context.device(), &view, nullptr, &image.view);
    if (result != VK_SUCCESS) {
        vkDestroyImage(context.device(), image.image, nullptr);
        vkFreeMemory(context.device(), image.memory, nullptr);
        return Result<VulkanImage>::failure(vk_error("vkCreateImageView", result));
    }
    return Result<VulkanImage>::success(image);
}

Result<VulkanImage> create_vulkan_depth_image(VulkanContext& context, VkExtent2D extent) {
    return create_vulkan_color_image(
        context, VK_FORMAT_D32_SFLOAT, extent,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
}

void destroy_vulkan_image(VulkanContext& context, VulkanImage& image) {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(context.device(), image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(context.device(), image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.device(), image.memory, nullptr);
        image.memory = VK_NULL_HANDLE;
    }
}

}  // namespace splat

#endif  // SPLAT_HAVE_VULKAN
