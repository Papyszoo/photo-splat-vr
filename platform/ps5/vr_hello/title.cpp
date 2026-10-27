// vr_hello as a homebrew title: the PSVR2 display path, RADV -> Hmd2.
//
// What it proves (Spike B step 3): a title renders a test pattern with
// RADV/Vulkan into three host-visible linear RGBA8 buffers, hands their
// vkMapMemory addresses to Hmd2 as eye textures, and flips them to the headset
// the way HLA-PSVR2 does -- EnableVrMode(0x2000f), a vblank gate, the 120 Hz
// output at half rate (SetRenderConfig(1)), pacing on Hmd2's predicted display
// time, and a re-submit of frames Hmd2 opened but never received an image for.
// It runs for 30 s, then ends through sceSystemServiceLoadExec("exit").
//
// The picture is drawn by a Vulkan pass (one full-screen triangle, pattern in
// the fragment shader) into a side-by-side R8G8B8A8_UNORM image, copied to the
// mapped buffer on the GPU, waited on with a bounded fence, then submitted. It
// is not CPU-written: the only CPU-side pixels are none at all.
//
// Left eye: green, a big orange "L"; right eye: blue, a big cyan "R"; both a
// 128-pixel grid, a white centre cross, corner squares and a yellow bar that
// moves with head yaw. A swapped eye, a single-eye picture, a dead pose path
// or a stale buffer is visible at a glance.
//
// The headset path is hmd2's; the eye-buffer handoff, pacing, vblank gate and
// abandoned-frame re-submit are HLA-PSVR2's (GPL-3.0-or-later, Copyright (C)
// 2026 theghostonline, ps5xr/runtime/src/{psvr2,xr_session,xr_vulkan}.c). See
// THIRD_PARTY_NOTICES.md. The Vulkan entry-point shims are the viewer title's
// (platform/ps5/viewer/vk_entrypoints.cpp): a title links RADV directly and has
// no loader.
//
// Bounded waits, leak-on-timeout: the per-frame fence wait is 500 ms. If it
// times out the GPU may still be reading the RADV objects, so the display and
// Hmd2 are still shut down (the headset must not be left in VR mode) but the
// Vulkan objects are deliberately leaked; the log says so and the title ends.
//
// Build: make -C platform/ps5/vr_hello vr-hello-title
//        -> build/ps5/title/PPSA99202/ (the one app; it replaces the viewer's eboot)
// Run:   platform/ps5/scripts/run-vr-hello.sh   (docs/console-runbook.md 1c)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>

#include <sys/sysctl.h>

#include <ps5platform/klog.h>

#include <vulkan/vulkan.h>

#include "../hmd2/hmd2.h"
// The eye buffers vr_hello handed to Hmd2 by hand in Spike B step 3; now the
// shared implementation the viewer title's VR mode uses too (structure and
// allocation identical, the copy recorded by record_eye_copy).
#include "../viewer/eye_buffers.h"
// The Vulkan result name and memory-type search, one copy for every title
// (the viewer's eye buffers use the same two).
#include "../viewer/vk_common.h"

#include "pattern_frag.h"
#include "pattern_vert.h"

extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceSystemServiceLoadExec(const char* path, const char* const* args);
extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceKernelUsleep(unsigned microseconds);

namespace {

// The shared Vulkan helpers (../viewer/vk_common.h): vk_name() for every log
// line below, find_memory_type() for the pattern image.
using splat::ps5::find_memory_type;
using splat::ps5::vk_name;

// HLA-PSVR2 ships 1984x2048 per eye at 60 rendered into 120 Hz output
// (ps5xr/runtime/src/xr_instance.c eye_size, vrtitle/ps5xr.ini eye_scale 1.0:
// the panel is 2000x2040, rounded down to whole 64-pixel texels).
constexpr uint32_t kEyeWidth = 1984;
constexpr uint32_t kEyeHeight = 2048;
constexpr uint32_t kSideBySide = 2 * kEyeWidth;
constexpr uint32_t kBuffers = 3;
constexpr uint64_t kImageBytes = static_cast<uint64_t>(kSideBySide) * kEyeHeight * 4;
constexpr uint64_t kRunUs = 30ull * 1000 * 1000;
constexpr uint64_t kFenceTimeoutNs = 500ull * 1000 * 1000;
constexpr int kMinimumFrames = 20 * 30;   // below this the loop is not keeping up
constexpr float kYawFullScaleDeg = 45.0f;
// title.sh stop writes this; the loop checks it once a second. A title cannot
// read /data until ShadowMountPlus's mount is there, so a failure to stat is
// simply "no request" and never ends the run.
constexpr const char* kQuitPath = "/data/ps5-splat-vr/title-quit";

void line(const char* format, ...) __attribute__((format(printf, 1, 2)));
void line(const char* format, ...) {
    char message[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    char out[800];
    std::snprintf(out, sizeof(out), "%s\n", message);
    sceKernelDebugOutText(0, out);
}

uint64_t now_us() {
    return sceKernelGetProcessTime();
}

double now_ms() {
    return static_cast<double>(now_us()) / 1000.0;
}

void log_firmware() {
    uint32_t sdk = 0;
    size_t size = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &size, nullptr, 0) == 0 && sdk)
        line("vrhello: firmware %x.%02x (kern.sdk_version 0x%08x)", sdk >> 24, (sdk >> 16) & 0xff,
             sdk);
    else
        line("vrhello: firmware unknown (kern.sdk_version unavailable)");
}

// Head yaw in degrees from the pose's rotation (forward = R * (0,0,-1)).
double head_yaw(const hmd2::Pose& pose) {
    const float x = pose.quat[0], y = pose.quat[1], z = pose.quat[2], w = pose.quat[3];
    const float fx = -2.0f * (x * z + y * w);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    return static_cast<double>(std::atan2(fx, -fz) * 57.29578f);
}

// -- Vulkan -----------------------------------------------------------------
struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[kBuffers] = {};
    VkFence fences[kBuffers] = {};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    bool image_ready = false;   // the first frame records UNDEFINED -> GENERAL
    splat::ps5::EyeBuffers eye_buffers;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Push {
    uint32_t tw;
    uint32_t eye_w;
    uint32_t eye_h;
    float marker;
};

// The device and queue, the way the viewer brings RADV up: 1.3, dynamic
// rendering (the pattern's one pass needs no render pass object).
bool create_device(Gpu& gpu) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vr_hello title";
    app.pEngineName = "ps5-splat-vr";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &gpu.instance);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateInstance -> %s", vk_name(result));
        return false;
    }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        line("vrhello: FAIL no Vulkan device (0x%08x, %u)", static_cast<unsigned>(result), count);
        return false;
    }
    gpu.physical = VK_NULL_HANDLE;
    VkPhysicalDevice devices[8] = {};
    count = count > 8 ? 8 : count;
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (features13.dynamicRendering && properties.apiVersion >= VK_API_VERSION_1_3) {
            gpu.physical = devices[i];
            line("vrhello: device %s, Vulkan %u.%u.%u", properties.deviceName,
                 VK_API_VERSION_MAJOR(properties.apiVersion),
                 VK_API_VERSION_MINOR(properties.apiVersion),
                 VK_API_VERSION_PATCH(properties.apiVersion));
            break;
        }
    }
    if (gpu.physical == VK_NULL_HANDLE) {
        line("vrhello: FAIL no Vulkan 1.3 device with dynamic rendering");
        return false;
    }
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, nullptr);
    VkQueueFamilyProperties queue_families[16] = {};
    families = families > 16 ? 16 : families;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, queue_families);
    gpu.family = UINT32_MAX;
    for (uint32_t i = 0; i < families; i++)
        if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            gpu.family = i;
            break;
        }
    if (gpu.family == UINT32_MAX) {
        line("vrhello: FAIL no graphics+compute queue family");
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = gpu.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features13;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    result = vkCreateDevice(gpu.physical, &device_info, nullptr, &gpu.device);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateDevice -> %s", vk_name(result));
        return false;
    }
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &gpu.memory);
    line("vrhello: memory: %u heaps, %u types", gpu.memory.memoryHeapCount,
         gpu.memory.memoryTypeCount);

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu.family;
    result = vkCreateCommandPool(gpu.device, &pool, nullptr, &gpu.pool);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateCommandPool -> %s", vk_name(result));
        return false;
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = gpu.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = kBuffers;
    result = vkAllocateCommandBuffers(gpu.device, &allocate, gpu.commands);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkAllocateCommandBuffers -> %s", vk_name(result));
        return false;
    }
    for (uint32_t i = 0; i < kBuffers; i++) {
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(gpu.device, &fence, nullptr, &gpu.fences[i]);
        if (result != VK_SUCCESS) {
            line("vrhello: FAIL vkCreateFence -> %s", vk_name(result));
            return false;
        }
    }
    return true;
}

bool create_shader_module(Gpu& gpu, const uint32_t* code, size_t bytes, VkShaderModule* out,
                          const char* name) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    const VkResult result = vkCreateShaderModule(gpu.device, &info, nullptr, out);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateShaderModule(%s) -> %s", name, vk_name(result));
        return false;
    }
    return true;
}

bool create_pipeline(Gpu& gpu) {
    if (!create_shader_module(gpu, vrhello_pattern_vert, sizeof(vrhello_pattern_vert), &gpu.vert,
                              "pattern.vert") ||
        !create_shader_module(gpu, vrhello_pattern_frag, sizeof(vrhello_pattern_frag), &gpu.frag,
                              "pattern.frag"))
        return false;
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(Push);
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    VkResult result = vkCreatePipelineLayout(gpu.device, &layout, nullptr, &gpu.layout);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreatePipelineLayout -> %s", vk_name(result));
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = gpu.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = gpu.frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colour{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colour.attachmentCount = 1;
    colour.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &gpu.format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &colour;
    info.pDynamicState = &dynamic;
    info.layout = gpu.layout;
    result = vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &gpu.pipeline);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateGraphicsPipelines -> %s", vk_name(result));
        return false;
    }
    return true;
}

bool create_eye_image(Gpu& gpu) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = gpu.format;
    info.extent = {kSideBySide, kEyeHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(gpu.device, &info, nullptr, &gpu.image);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateImage -> %s", vk_name(result));
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(gpu.device, gpu.image, &requirements);
    const uint32_t type = find_memory_type(gpu.memory, requirements.memoryTypeBits, 0,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    result = vkAllocateMemory(gpu.device, &allocate, nullptr, &gpu.image_memory);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkAllocateMemory(image) -> %s", vk_name(result));
        return false;
    }
    result = vkBindImageMemory(gpu.device, gpu.image, gpu.image_memory, 0);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkBindImageMemory -> %s", vk_name(result));
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = gpu.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = gpu.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = vkCreateImageView(gpu.device, &view, nullptr, &gpu.view);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkCreateImageView -> %s", vk_name(result));
        return false;
    }
    // The layout transition (UNDEFINED -> GENERAL) is recorded at the start of
    // the first frame, in the same command buffer as that frame: no separate
    // one-off submission, and the only GPU wait in the program stays the
    // per-frame fence below.
    return true;
}

// Three host-visible, CPU-mapped buffers: the eye images Hmd2 samples. The
// shared EyeBuffers class (platform/ps5/viewer/eye_buffers.h) does the same
// allocation this file used to do inline: prefer DEVICE_LOCAL among the
// host-visible coherent types (on this console the winsys maps direct memory at
// one address for the CPU and the GPU), zero it, and refuse a non-256-byte
// aligned address.
bool create_eye_buffers(Gpu& gpu) {
    std::string error;
    if (!gpu.eye_buffers.create(gpu.device, gpu.memory, kBuffers, kSideBySide, kEyeHeight, error)) {
        line("vrhello: FAIL eye buffers: %s", error.c_str());
        return false;
    }
    for (int i = 0; i < gpu.eye_buffers.count(); i++) {
        line("vrhello: eye buffer %d: %ux%u RGBA8 at %p (%llu bytes, memory type %u)", i,
             gpu.eye_buffers.width(), gpu.eye_buffers.height(), gpu.eye_buffers.mapped(i),
             static_cast<unsigned long long>(gpu.eye_buffers.bytes()),
             gpu.eye_buffers.memory_type(i));
    }
    return true;
}

// Record, submit and wait for one frame. The wait is bounded; false means the
// GPU did not finish and nothing may be freed under it.
bool render_frame(Gpu& gpu, uint32_t index, float marker, double* wait_ms) {
    VkCommandBuffer commands = gpu.commands[index];
    VkResult result = vkResetCommandBuffer(commands, 0);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkResetCommandBuffer -> %s", vk_name(result));
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(commands, &begin);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkBeginCommandBuffer -> %s", vk_name(result));
        return false;
    }
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(kSideBySide),
                              static_cast<float>(kEyeHeight), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {kSideBySide, kEyeHeight}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);

    if (!gpu.image_ready) {
        VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcAccessMask = 0;
        to_general.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        to_general.image = gpu.image;
        to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_general);
        gpu.image_ready = true;
    }

    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView = gpu.view;
    colour.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, {kSideBySide, kEyeHeight}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colour;
    vkCmdBeginRendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, gpu.pipeline);
    const Push push{kSideBySide, kEyeWidth, kEyeHeight, marker};
    vkCmdPushConstants(commands, gpu.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRendering(commands);

    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_copy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_copy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_copy.image = gpu.image;
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);

    // Copy into the eye buffer Hmd2 will scan out, with the barrier that makes
    // it visible to Hmd2 before the flip (shared with the viewer's VR mode).
    splat::ps5::record_eye_copy(commands, gpu.image, VK_IMAGE_LAYOUT_GENERAL,
                                gpu.eye_buffers.buffer(static_cast<int>(index)), kSideBySide,
                                kEyeHeight);
    result = vkEndCommandBuffer(commands);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkEndCommandBuffer -> %s", vk_name(result));
        return false;
    }
    vkResetFences(gpu.device, 1, &gpu.fences[index]);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    result = vkQueueSubmit(gpu.queue, 1, &submit, gpu.fences[index]);
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkQueueSubmit -> %s", vk_name(result));
        return false;
    }
    const double started = now_ms();
    result = vkWaitForFences(gpu.device, 1, &gpu.fences[index], VK_TRUE, kFenceTimeoutNs);
    if (wait_ms != nullptr) *wait_ms = now_ms() - started;
    if (result != VK_SUCCESS) {
        line("vrhello: FAIL vkWaitForFences -> %s (500 ms)", vk_name(result));
        return false;
    }
    return true;
}

void destroy_gpu(Gpu& gpu) {
    // Only called when no fence timed out: every submission is known finished.
    if (gpu.device != VK_NULL_HANDLE) {
        gpu.eye_buffers.destroy();
        for (uint32_t i = 0; i < kBuffers; i++) {
            if (gpu.fences[i] != VK_NULL_HANDLE) vkDestroyFence(gpu.device, gpu.fences[i], nullptr);
        }
        if (gpu.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(gpu.device, gpu.pipeline, nullptr);
        if (gpu.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(gpu.device, gpu.layout, nullptr);
        if (gpu.frag != VK_NULL_HANDLE) vkDestroyShaderModule(gpu.device, gpu.frag, nullptr);
        if (gpu.vert != VK_NULL_HANDLE) vkDestroyShaderModule(gpu.device, gpu.vert, nullptr);
        if (gpu.view != VK_NULL_HANDLE) vkDestroyImageView(gpu.device, gpu.view, nullptr);
        if (gpu.image != VK_NULL_HANDLE) vkDestroyImage(gpu.device, gpu.image, nullptr);
        if (gpu.image_memory != VK_NULL_HANDLE)
            vkFreeMemory(gpu.device, gpu.image_memory, nullptr);
        if (gpu.pool != VK_NULL_HANDLE) vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
        vkDestroyDevice(gpu.device, nullptr);
    }
    if (gpu.instance != VK_NULL_HANDLE) vkDestroyInstance(gpu.instance, nullptr);
    // Not `gpu = Gpu{}`: Gpu holds the eye buffers, whose class deletes its copy
    // assignment (a copy would double-free the mapped memory).
    gpu.instance = VK_NULL_HANDLE;
    gpu.physical = VK_NULL_HANDLE;
    gpu.device = VK_NULL_HANDLE;
    gpu.queue = VK_NULL_HANDLE;
    gpu.pool = VK_NULL_HANDLE;
    gpu.image = VK_NULL_HANDLE;
    gpu.image_memory = VK_NULL_HANDLE;
    gpu.view = VK_NULL_HANDLE;
    gpu.image_ready = false;
    gpu.vert = VK_NULL_HANDLE;
    gpu.frag = VK_NULL_HANDLE;
    gpu.layout = VK_NULL_HANDLE;
    gpu.pipeline = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kBuffers; i++) {
        gpu.commands[i] = VK_NULL_HANDLE;
        gpu.fences[i] = VK_NULL_HANDLE;
    }
}

[[noreturn]] void end_title(int status, const char* reason) {
    line("vrhello: %s, exit %d", reason, status);
    line("vrhello: done, exit %d", status);
    sceSystemServiceLoadExec("exit", nullptr);
    // LoadExec does not return; a return would fall back into the title CRT.
    for (;;) sceKernelUsleep(1000000);
}

}  // namespace

// app_crt's _start calls this as main(). A title launched through ProsperoMgr
// gets no arguments; the run length is compiled in (30 s, the task's window).
int main(int, char**) {
#ifndef SPLAT_TITLE_ID
#define SPLAT_TITLE_ID "unknown"
#endif
    line("vrhello: start (vr_hello title %s, RADV eye buffers, 30 s)", SPLAT_TITLE_ID);
    sceSystemServiceHideSplashScreen();
    log_firmware();
    line("vrhello: requested mode 120 Hz output half-rate (EnableVrMode 0x2000f + "
         "SetRenderConfig(1)), eye %ux%u, side-by-side %ux%u, %u buffers",
         kEyeWidth, kEyeHeight, kSideBySide, kEyeHeight, kBuffers);
    // RADV's title setting, before the first GPU call (PS5LM's order).
    setenv("radv_enable_unified_heap_on_apu", "true", 0);
    const char* unified = getenv("radv_enable_unified_heap_on_apu");
    line("vrhello: radv_enable_unified_heap_on_apu=%s", unified != nullptr ? unified : "true");
    line("vrhello: driver stderr capture %s",
         ps5_klog_capture_stderr("[radv]") == 0 ? "on" : "off");

    Gpu gpu;
    if (!create_device(gpu)) {
        destroy_gpu(gpu);
        end_title(2, "FAIL no Vulkan device");
    }
    if (!create_pipeline(gpu)) {
        destroy_gpu(gpu);
        end_title(3, "FAIL no pattern pipeline");
    }
    if (!create_eye_image(gpu)) {
        destroy_gpu(gpu);
        end_title(4, "FAIL no eye image");
    }
    if (!create_eye_buffers(gpu)) {
        destroy_gpu(gpu);
        end_title(5, "FAIL no eye buffers");
    }

    hmd2::set_logger([](const char* msg) { line("%s", msg); });
    hmd2::Config config;
    hmd2::ExternalImage external[kBuffers] = {};
    for (uint32_t i = 0; i < kBuffers; i++) {
        external[i].address = gpu.eye_buffers.mapped(static_cast<int>(i));
        external[i].bytes = static_cast<size_t>(kImageBytes);
    }
    config.external = external;
    config.external_count = static_cast<int>(kBuffers);
    config.image_width = kSideBySide;
    config.image_height = kEyeHeight;
    if (!hmd2::init(config)) {
        const hmd2::Error error = hmd2::error();
        line("vrhello: init failed at %s (rc 0x%08x) - see the hmd2 lines above",
             error.step != nullptr && error.step[0] != '\0' ? error.step : "unknown",
             static_cast<unsigned>(error.rc));
        hmd2::shutdown();
        destroy_gpu(gpu);
        end_title(6, "FAIL hmd2 init");
    }
    const hmd2::Fov fov = hmd2::field_of_view();
    line("vrhello: headset: per eye %ux%u, side-by-side %ux%u, %d buffers, linear RGBA8 %llu "
         "bytes each, refresh %d Hz, fov out %.3f in %.3f top %.3f bottom %.3f",
         kEyeWidth, kEyeHeight, hmd2::eye_size() * 2, hmd2::eye_height(), hmd2::buffer_count(),
         static_cast<unsigned long long>(hmd2::image_bytes()), hmd2::refresh_hz(),
         static_cast<double>(fov.out), static_cast<double>(fov.in),
         static_cast<double>(fov.top), static_cast<double>(fov.bottom));

    const uint64_t started = now_us();
    const std::time_t started_wall = std::time(nullptr);
    uint64_t next_log = started + 1000000;
    uint64_t next_quit_check = started + 1000000;
    uint64_t window_start = started;
    int window_frames = 0;
    int frames = 0;
    int failures = 0;
    int begin_failures = 0;
    bool gpu_ok = true;
    bool was_worn = hmd2::worn();
    bool removed = false;
    bool quit = false;
    double gpu_wait_sum = 0.0;
    int gpu_wait_count = 0;
    uint64_t first_frame_ms = 0;
    uint64_t shown = 0;
    const char* failure_reason = "";

    for (;;) {
        const uint64_t now = now_us();
        if (now - started >= kRunUs) break;
        if (!hmd2::wait_slot()) break;
        if (!hmd2::begin_frame()) {
            if (++begin_failures > 120) {
                failure_reason = "120 frames without a BeginFrame";
                break;
            }
            continue;
        }
        begin_failures = 0;
        const hmd2::EyeBuffer eye = hmd2::acquire();
        const hmd2::Pose pose = hmd2::predicted_pose();
        const double yaw = head_yaw(pose);
        float marker = static_cast<float>(0.5 + yaw / (2.0 * kYawFullScaleDeg));
        if (marker < 0.0f) marker = 0.0f;
        if (marker > 1.0f) marker = 1.0f;
        double wait_ms = 0.0;
        if (!render_frame(gpu, static_cast<uint32_t>(eye.index), marker, &wait_ms)) {
            gpu_ok = false;
            failure_reason = "GPU frame failed";
            break;
        }
        gpu_wait_sum += wait_ms;
        gpu_wait_count++;
        if (!hmd2::submit()) failures++;
        frames++;
        window_frames++;
        if (frames == 1) {
            first_frame_ms = (now_us() - started) / 1000;
            line("vrhello: first frame in %llu ms (buffer %d, VkImage %ux%u R8G8B8A8_UNORM, "
                 "GPU wait %.2f ms)",
                 static_cast<unsigned long long>(first_frame_ms), eye.index, kSideBySide, kEyeHeight,
                 wait_ms);
        }

        const bool worn = hmd2::worn();
        if (was_worn && !worn) {
            removed = true;
            line("vrhello: headset taken off after %d frames (still running to 30 s)", frames);
        }
        was_worn = worn;

        const uint64_t after = now_us();
        if (after >= next_quit_check) {
            struct stat st {};
            if (::stat(kQuitPath, &st) == 0 && st.st_mtime + 1 >= started_wall) {
                quit = true;
                line("vrhello: quit requested (%s); ending", kQuitPath);
                break;
            }
            next_quit_check += 1000000;
            if (next_quit_check < after) next_quit_check = after + 1000000;
        }
        if (after >= next_log) {
            const double seconds = static_cast<double>(after - window_start) / 1e6;
            const double fps = seconds > 0.0 ? static_cast<double>(window_frames) / seconds : 0.0;
            const hmd2::FlipInfo flip = hmd2::flip_status();
            const hmd2::PaceStats pace = hmd2::take_pace();
            const hmd2::DeviceState device = hmd2::device_state();
            shown = flip.shown;
            line("vrhello: t=%llus frames=%d fps=%.1f submitted=%d shown=%llu missed=%d repeat=%d "
                 "queue-full=%d err=%d pace[nap=%.2f call=%.2f ahead=%.2f step=%.2f ms] worn=%d "
                 "lens=%u gpu=%.2f ms",
                 static_cast<unsigned long long>((after - started) / 1000000), frames, fps,
                 hmd2::frames_submitted(), static_cast<unsigned long long>(flip.shown),
                 hmd2::missed_frames(), hmd2::repeated_frames(), hmd2::queue_full_waits(),
                 hmd2::frame_errors(), static_cast<double>(pace.nap_ms),
                 static_cast<double>(pace.call_ms), static_cast<double>(pace.ahead_ms),
                 static_cast<double>(pace.step_ms), device.worn ? 1 : 0, device.lens_mm,
                 gpu_wait_count != 0 ? gpu_wait_sum / gpu_wait_count : 0.0);
            window_frames = 0;
            window_start = after;
            gpu_wait_sum = 0.0;
            gpu_wait_count = 0;
            next_log += 1000000;
            if (next_log < after) next_log = after + 1000000;
        }
    }

    const uint64_t run_ms = (now_us() - started) / 1000;
    const hmd2::FlipInfo flip = hmd2::flip_status();
    shown = flip.shown;
    // Cache the counters before teardown resets them.
    const int final_errors = hmd2::frame_errors();
    const int final_missed = hmd2::missed_frames();
    const int final_repeated = hmd2::repeated_frames();
    const int final_queue_full = hmd2::queue_full_waits();
    const int final_submitted = hmd2::frames_submitted();
    // VideoOut's flip status on the VR port is not exercised by HLA-PSVR2, so
    // it is a bonus, not a gate: when the call works, at least one flip must
    // have been shown; when it does not, the frames and Hmd2's own errors are
    // the criterion and the picture in the headset is the human check.
    const bool pass = gpu_ok && frames >= kMinimumFrames && final_errors == 0 && failures == 0 &&
                      (flip.rc != 0 || shown > 0);
    double fps = run_ms > 0 ? static_cast<double>(frames) * 1000.0 / static_cast<double>(run_ms) : 0.0;
    line("vrhello: run: %d frames in %llu ms (%.1f fps), submitted=%d shown=%llu missed=%d "
         "repeat=%d queue-full=%d err=%d flip-rc=0x%08x",
         frames, static_cast<unsigned long long>(run_ms), fps, final_submitted,
         static_cast<unsigned long long>(shown), final_missed, final_repeated, final_queue_full,
         final_errors, static_cast<unsigned>(flip.rc));

    // Teardown, in this order: the display and Hmd2 first (so the headset is
    // not left in VR mode and nothing samples the eye buffers), then Vulkan.
    // A GPU wait that timed out means the device may still read them: the
    // Vulkan objects are leaked deliberately rather than freed under it.
    const double teardown_started = now_ms();
    line("vrhello: teardown: closing the VR display and Hmd2");
    hmd2::shutdown();
    if (gpu_ok) {
        destroy_gpu(gpu);
        line("vrhello: teardown: RADV objects destroyed");
    } else {
        line("vrhello: teardown: leaking the Vulkan image, buffers and device (a GPU wait timed "
             "out; nothing may be freed under it)");
    }
    line("vrhello: teardown: done in %.1f ms", now_ms() - teardown_started);

    if (quit) end_title(0, "quit requested");
    if (pass) {
        line("vrhello: PASS %d frames, %.1f fps, shown %llu, missed %d, headset %s", frames, fps,
             static_cast<unsigned long long>(shown), final_missed,
             removed ? "taken off during the run" : (was_worn ? "worn" : "not worn"));
        end_title(0, "PASS");
    }
    line("vrhello: FAIL %d frames, %.1f fps, gpu_ok=%d shown=%llu err=%d %s", frames, fps,
         gpu_ok ? 1 : 0, static_cast<unsigned long long>(shown), final_errors,
         failure_reason);
    end_title(1, "FAIL");
}
