// Spike B, step 1: RADV on the PS5, linked directly (no Vulkan loader).
//
// Creates an instance and a device through RADV's vk_icdGetInstanceProcAddr,
// logs the device, its queue families and its memory heaps (the PS5 winsys
// should report one unified heap of about 11.44 GiB), then runs the project's
// fill compute shader (core/render/shaders/fill.comp, compiled to SPIR-V and
// embedded here), reads the buffer back over host-coherent memory and checks
// that every element is i * 3 + 7.
//
// The device comes up as PS5LM brings it up (radv_enable_unified_heap_on_apu=1,
// one heap over the direct-memory pool), and teardown waits for the submission
// with a bound: if the GPU did not finish, it leaks rather than frees.
//
// Every line goes to stdout (elfldr streams it back over its socket) and to
// the kernel log via klog_puts, which is what a launch through ProsperoMgr
// leaves observable. Exit value 0 means PASS, 1 means FAIL.
//
// Built with -DPS5_TITLE the same source is the title's eboot
// (platform/ps5/title/): the same code path, with the four differences a title
// process forces (docs/research/spike-b-memory.md sections 5-6, 9):
//   - it logs through sceKernelDebugOutText, because no one reads a title's
//     stdout, and the message text is unchanged so the klog grep is the same;
//   - it prints the direct-memory pool (and the flexible pool) before creating
//     the device: the pool is the title's, and RADV's heap is that number;
//   - RADV's shader cache is left at its title default (/app0/radv-shader-cache)
//     instead of being disabled as a payload must;
//   - it never returns to the CRT: it ends the app with
//     sceSystemServiceLoadExec("exit", nullptr), as PS5LM does.
//
// Build: make -C platform/ps5/vk_hello vk_hello.elf      (payload)
//        make -C platform/ps5/title                      (title package)
// Run:   platform/ps5/scripts/run.sh platform/ps5/vk_hello/vk_hello.elf 20
//        platform/ps5/scripts/title.sh launch build/ps5/title/PPSA99202
//
// The start-up order follows PS5_Vulkan's radv_smoke.c (GPL-3.0-or-later):
// vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") is the only
// entry point a program linking the archive gets without a loader.
// SPDX-License-Identifier: GPL-3.0-or-later

#define VK_NO_PROTOTYPES

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/types.h>
#include <sys/sysctl.h>

#ifdef PS5_TITLE
#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

extern "C" int sceSystemServiceHideSplashScreen(void);
// How a title process ends: a title may not exit() (SIGSYS).
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *args);
#else
#include <ps5/klog.h>
#include <ps5platform/klog.h>
#endif

// After the system headers: the SDK's <sys/sysctl.h> needs u_int from
// <sys/types.h>, and the Vulkan header's own system includes come second best.
#include <vulkan/vulkan.h>

#include "fill_spv.h"

// RADV's ICD entry, exported from libvulkan_radeon.ps5.a.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

namespace {

// ------------------------------------------------------------------ logging

void
line(const char *fmt, ...) {
    char    msg[512];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

#ifdef PS5_TITLE
    // No stdout reader in a title (elfldr's socket belongs to payloads): klog
    // is the only live sink. Same text as the payload's line, so the runbook's
    // greps and expected lines are shared.
    char klog_msg[544];
    std::snprintf(klog_msg, sizeof(klog_msg), "%s\n", msg);
    sceKernelDebugOutText(0, klog_msg);
#else
    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
#endif
}

#ifdef PS5_TITLE
double
gib(int64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

// The two pools a title process has, read never assumed
// (docs/research/spike-b-memory.md sections 1-2): direct memory is what RADV's
// PS5 winsys turns into its one device heap, flexible memory is libc's budget.
// A payload gets no direct pool at all, which is why the payload build's heap
// is empty.
void
log_pools() {
    const int64_t direct    = sceKernelGetDirectMemorySize();
    size_t        available = 0;
    int64_t       physical  = 0;
    const int32_t rv        = sceKernelAvailableDirectMemorySize(0, direct, 2 * 1024 * 1024,
                                                                 &physical, &available);
    line("vk_hello: direct pool %lld bytes (%.2f GiB), available %zu bytes (%.2f GiB) rv=0x%08x",
         (long long)direct, gib(direct), available, gib((int64_t)available), (uint32_t)rv);

    size_t        configured    = 0;
    size_t        free_flexible = 0;
    const int32_t rv_configured = sceKernelConfiguredFlexibleMemorySize(&configured);
    const int32_t rv_free       = sceKernelAvailableFlexibleMemorySize(&free_flexible);
    line("vk_hello: flexible pool %zu MiB configured rv=0x%08x, %zu MiB free rv=0x%08x",
         configured / (1024 * 1024), (uint32_t)rv_configured, free_flexible / (1024 * 1024),
         (uint32_t)rv_free);
}
#endif

double
now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

const char *
result_name(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VkResult";
    }
}

// ----------------------------------------------------------- function table

#define INSTANCE_COMMANDS(X)                                                       \
    X(DestroyInstance)                                                             \
    X(EnumeratePhysicalDevices)                                                    \
    X(GetPhysicalDeviceProperties)                                                 \
    X(GetPhysicalDeviceQueueFamilyProperties)                                      \
    X(GetPhysicalDeviceMemoryProperties)                                           \
    X(CreateDevice)                                                                \
    X(GetDeviceProcAddr)

#define DEVICE_COMMANDS(X)                                                         \
    X(DestroyDevice)                                                               \
    X(GetDeviceQueue)                                                              \
    X(CreateBuffer)                                                                \
    X(DestroyBuffer)                                                               \
    X(GetBufferMemoryRequirements)                                                 \
    X(AllocateMemory)                                                              \
    X(FreeMemory)                                                                  \
    X(BindBufferMemory)                                                            \
    X(MapMemory)                                                                   \
    X(UnmapMemory)                                                                 \
    X(FlushMappedMemoryRanges)                                                     \
    X(InvalidateMappedMemoryRanges)                                                \
    X(CreateShaderModule)                                                          \
    X(DestroyShaderModule)                                                         \
    X(CreateDescriptorSetLayout)                                                   \
    X(DestroyDescriptorSetLayout)                                                  \
    X(CreatePipelineLayout)                                                        \
    X(DestroyPipelineLayout)                                                       \
    X(CreateComputePipelines)                                                      \
    X(DestroyPipeline)                                                             \
    X(CreateDescriptorPool)                                                        \
    X(DestroyDescriptorPool)                                                       \
    X(AllocateDescriptorSets)                                                      \
    X(UpdateDescriptorSets)                                                        \
    X(CreateCommandPool)                                                           \
    X(DestroyCommandPool)                                                          \
    X(AllocateCommandBuffers)                                                      \
    X(FreeCommandBuffers)                                                          \
    X(BeginCommandBuffer)                                                          \
    X(EndCommandBuffer)                                                            \
    X(CmdBindPipeline)                                                             \
    X(CmdBindDescriptorSets)                                                       \
    X(CmdDispatch)                                                                 \
    X(CmdPipelineBarrier)                                                          \
    X(QueueSubmit)                                                                 \
    X(QueueWaitIdle)                                                               \
    X(CreateFence)                                                                 \
    X(DestroyFence)                                                                \
    X(ResetFences)                                                                 \
    X(WaitForFences)

#define DECLARE(name) PFN_vk##name pfn_##name = nullptr;
INSTANCE_COMMANDS(DECLARE)
DEVICE_COMMANDS(DECLARE)
#undef DECLARE
PFN_vkCreateInstance pfn_CreateInstance = nullptr;

#define CLEAR(name) pfn_##name = nullptr;
void
clear_table() {
    INSTANCE_COMMANDS(CLEAR)
    DEVICE_COMMANDS(CLEAR)
    pfn_CreateInstance = nullptr;
}
#undef CLEAR

// --------------------------------------------------------------- Vulkan state

struct State {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize buffer_bytes = 0;

    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

State state;

// How long teardown may wait for an outstanding submission before it gives up.
constexpr uint64_t kTeardownWaitMs = 2000;

// True from just before vkQueueSubmit until the fence has signalled. While it
// is set, a free or a destroy could pull memory or a device out from under a
// dispatch the GPU may still be running.
bool gpu_may_run = false;

// True once a batch reached vkQueueSubmit; only then is there a queue to wait
// for at teardown (and only after the fence confirmed it, so it cannot block).
bool gpu_submitted = false;

// Did the last submission finish? Waits on its fence for a bounded time.
// VK_TIMEOUT (still running) and VK_ERROR_DEVICE_LOST (unknown) both mean the
// GPU's state is unknown, and the caller must not free anything.
bool
gpu_idle(const char *when) {
    if (state.device == VK_NULL_HANDLE || !gpu_may_run)
        return true;
    if (!pfn_WaitForFences || state.fence == VK_NULL_HANDLE) {
        line("vk_hello: %s: submission outstanding but no fence to wait on; the GPU may still run", when);
        return false;
    }
    const double start = now_ms();
    const VkResult result =
        pfn_WaitForFences(state.device, 1, &state.fence, VK_TRUE, kTeardownWaitMs * UINT64_C(1000000));
    const double waited = now_ms() - start;
    if (result != VK_SUCCESS) {
        line("vk_hello: %s: GPU did not finish in %llu ms -> %s; the GPU may still run", when,
             (unsigned long long)kTeardownWaitMs, result_name(result));
        return false;
    }
    gpu_may_run = false;
    line("vk_hello: %s: GPU idle after %.1f ms", when, waited);
    return true;
}

void
destroy_all() {
    if (!gpu_idle("teardown")) {
        // Deliberate leak: an unfinished submission may still read the buffer
        // or the descriptors, so nothing is destroyed or freed here. The
        // process exits immediately after; whatever the driver and the OS can
        // reclaim, they reclaim.
        line("vk_hello: teardown: leaking device, queue, buffer and memory (no destroy or free)");
        clear_table();
        return;
    }
    if (state.device != VK_NULL_HANDLE) {
        // The fence above proved the submission done, so this is the driver's
        // own confirmation and returns at once rather than blocking on work.
        // Nothing was submitted on an early failure, and then there is no queue
        // wait at all: only the fence above may decide that waiting is bounded.
        if (gpu_submitted && pfn_QueueWaitIdle && state.queue) {
            const VkResult result = pfn_QueueWaitIdle(state.queue);
            if (result != VK_SUCCESS)
                line("vk_hello: vkQueueWaitIdle -> %s", result_name(result));
        }
        if (pfn_DestroyFence && state.fence)
            pfn_DestroyFence(state.device, state.fence, nullptr);
        if (pfn_FreeCommandBuffers && state.command_buffer && state.command_pool)
            pfn_FreeCommandBuffers(state.device, state.command_pool, 1, &state.command_buffer);
        if (pfn_DestroyCommandPool && state.command_pool)
            pfn_DestroyCommandPool(state.device, state.command_pool, nullptr);
        if (pfn_DestroyDescriptorPool && state.descriptor_pool)
            pfn_DestroyDescriptorPool(state.device, state.descriptor_pool, nullptr);
        if (pfn_DestroyPipeline && state.pipeline)
            pfn_DestroyPipeline(state.device, state.pipeline, nullptr);
        if (pfn_DestroyPipelineLayout && state.pipeline_layout)
            pfn_DestroyPipelineLayout(state.device, state.pipeline_layout, nullptr);
        if (pfn_DestroyDescriptorSetLayout && state.set_layout)
            pfn_DestroyDescriptorSetLayout(state.device, state.set_layout, nullptr);
        if (pfn_DestroyShaderModule && state.shader)
            pfn_DestroyShaderModule(state.device, state.shader, nullptr);
        if (pfn_UnmapMemory && state.mapped)
            pfn_UnmapMemory(state.device, state.memory);
        state.mapped = nullptr;
        if (pfn_DestroyBuffer && state.buffer)
            pfn_DestroyBuffer(state.device, state.buffer, nullptr);
        if (pfn_FreeMemory && state.memory)
            pfn_FreeMemory(state.device, state.memory, nullptr);
        if (pfn_DestroyDevice)
            pfn_DestroyDevice(state.device, nullptr);
        state.device = VK_NULL_HANDLE;
    }
    if (pfn_DestroyInstance && state.instance != VK_NULL_HANDLE) {
        pfn_DestroyInstance(state.instance, nullptr);
        state.instance = VK_NULL_HANDLE;
    }
    clear_table();
}

// --------------------------------------------------------------- start-up

bool
load_instance_commands(VkInstance instance) {
    bool ok = true;
#define LOAD(name)                                                                 \
    pfn_##name = reinterpret_cast<PFN_vk##name>(                                   \
        vk_icdGetInstanceProcAddr(instance, "vk" #name));                          \
    if (!pfn_##name) {                                                             \
        line("vk_hello: FAIL missing %s", "vk" #name);                              \
        ok = false;                                                                \
    }
    INSTANCE_COMMANDS(LOAD)
#undef LOAD
    return ok;
}

bool
load_device_commands(VkDevice device) {
    bool ok = true;
#define LOAD(name)                                                                 \
    pfn_##name = reinterpret_cast<PFN_vk##name>(pfn_GetDeviceProcAddr(device, "vk" #name)); \
    if (!pfn_##name) {                                                             \
        line("vk_hello: FAIL missing %s", "vk" #name);                              \
        ok = false;                                                                \
    }
    DEVICE_COMMANDS(LOAD)
#undef LOAD
    return ok;
}

const char *
queue_flags_name(VkQueueFlags flags, char *buffer, size_t size) {
    std::snprintf(buffer, size, "%s%s%s%s", (flags & VK_QUEUE_GRAPHICS_BIT) ? "graphics " : "",
                  (flags & VK_QUEUE_COMPUTE_BIT) ? "compute " : "",
                  (flags & VK_QUEUE_TRANSFER_BIT) ? "transfer " : "",
                  (flags & VK_QUEUE_SPARSE_BINDING_BIT) ? "sparse" : "");
    return buffer;
}

bool
create_instance_and_device() {
    pfn_CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!pfn_CreateInstance) {
        line("vk_hello: FAIL vk_icdGetInstanceProcAddr has no vkCreateInstance");
        return false;
    }

    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ps5-splat-vr vk_hello",
        .applicationVersion = 1,
        .pEngineName = "ps5-splat-vr",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = 0,
        .enabledExtensionCount = 0,
    };
    VkResult result = pfn_CreateInstance(&instance_info, nullptr, &state.instance);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateInstance -> %s (%d)", result_name(result), (int)result);
        return false;
    }
    line("vk_hello: instance created (Vulkan 1.1 request, no extensions)");
    if (!load_instance_commands(state.instance))
        return false;

    uint32_t device_count = 0;
    result = pfn_EnumeratePhysicalDevices(state.instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        line("vk_hello: FAIL vkEnumeratePhysicalDevices -> %s, %u device(s)", result_name(result),
             device_count);
        return false;
    }
    line("vk_hello: physical devices: %u", device_count);
    result = pfn_EnumeratePhysicalDevices(state.instance, &device_count, &state.physical);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        line("vk_hello: FAIL vkEnumeratePhysicalDevices(devices) -> %s", result_name(result));
        return false;
    }

    VkPhysicalDeviceProperties properties{};
    pfn_GetPhysicalDeviceProperties(state.physical, &properties);
    line("vk_hello: device: %s", properties.deviceName);
    line("vk_hello: vendor 0x%04x device 0x%04x type %d", properties.vendorID, properties.deviceID,
         (int)properties.deviceType);
    line("vk_hello: api %u.%u.%u driver 0x%08x", VK_API_VERSION_MAJOR(properties.apiVersion),
         VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
         properties.driverVersion);

    uint32_t family_count = 0;
    pfn_GetPhysicalDeviceQueueFamilyProperties(state.physical, &family_count, nullptr);
    if (family_count == 0) {
        line("vk_hello: FAIL no queue families");
        return false;
    }
    VkQueueFamilyProperties families[16]{};
    if (family_count > 16)
        family_count = 16;
    pfn_GetPhysicalDeviceQueueFamilyProperties(state.physical, &family_count, families);
    line("vk_hello: queue families: %u", family_count);
    for (uint32_t i = 0; i < family_count; i++) {
        char flags[64];
        line("vk_hello: family[%u] queues %u flags %s timestamp bits %u", i, families[i].queueCount,
             queue_flags_name(families[i].queueFlags, flags, sizeof(flags)), families[i].timestampValidBits);
        if (state.queue_family == UINT32_MAX &&
            (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            state.queue_family = i;
    }
    if (state.queue_family == UINT32_MAX) {
        for (uint32_t i = 0; i < family_count; i++) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                state.queue_family = i;
                break;
            }
        }
    }
    if (state.queue_family == UINT32_MAX) {
        line("vk_hello: FAIL no compute queue family");
        return false;
    }
    line("vk_hello: using family[%u]", state.queue_family);

    VkPhysicalDeviceMemoryProperties memory{};
    pfn_GetPhysicalDeviceMemoryProperties(state.physical, &memory);
    line("vk_hello: memory: %u heaps, %u types", memory.memoryHeapCount, memory.memoryTypeCount);
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
        const VkMemoryHeap &heap = memory.memoryHeaps[i];
        line("vk_hello: heap[%u] %.2f GiB (%llu bytes) flags 0x%x%s", i,
             (double)heap.size / (1024.0 * 1024.0 * 1024.0), (unsigned long long)heap.size, heap.flags,
             (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    }
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        const VkMemoryType &type = memory.memoryTypes[i];
        line("vk_hello: type[%u] heap %u flags 0x%x%s%s%s%s", i, type.heapIndex, type.propertyFlags,
             (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device-local" : "",
             (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " host-visible" : "",
             (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " host-coherent" : "",
             (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " host-cached" : "");
    }
    VkDeviceSize local_bytes = 0;
    uint32_t local_heaps = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++) {
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            local_bytes += memory.memoryHeaps[i].size;
            local_heaps++;
        }
    }
    line("vk_hello: heap layout: %u heap(s), %u type(s), device-local %.2f GiB in %u heap(s)",
         memory.memoryHeapCount, memory.memoryTypeCount,
         (double)local_bytes / (1024.0 * 1024.0 * 1024.0), local_heaps);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = state.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledLayerCount = 0,
        .enabledExtensionCount = 0,
        .pEnabledFeatures = nullptr,
    };
    result = pfn_CreateDevice(state.physical, &device_info, nullptr, &state.device);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateDevice -> %s (%d)", result_name(result), (int)result);
        return false;
    }
    line("vk_hello: device created");
    if (!load_device_commands(state.device))
        return false;
    pfn_GetDeviceQueue(state.device, state.queue_family, 0, &state.queue);
    return true;
}

// ------------------------------------------------------------------- compute

bool
create_buffer_and_memory(uint32_t count) {
    state.buffer_bytes = (VkDeviceSize)count * sizeof(uint32_t);
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = state.buffer_bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = pfn_CreateBuffer(state.device, &info, nullptr, &state.buffer);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateBuffer -> %s", result_name(result));
        return false;
    }

    VkMemoryRequirements requirements{};
    pfn_GetBufferMemoryRequirements(state.device, state.buffer, &requirements);

    VkPhysicalDeviceMemoryProperties memory{};
    pfn_GetPhysicalDeviceMemoryProperties(state.physical, &memory);
    uint32_t type = UINT32_MAX;
    bool coherent = false;
    for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++) {
        const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            (pass == 0 ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : (VkMemoryPropertyFlags)0);
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                coherent = (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                break;
            }
        }
    }
    if (type == UINT32_MAX) {
        line("vk_hello: FAIL no host-visible memory type for a storage buffer");
        return false;
    }
    line("vk_hello: buffer %.1f KiB, memory type %u%s", (double)state.buffer_bytes / 1024.0, type,
         coherent ? " (host-coherent)" : " (host-visible, explicit flush)");

    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = pfn_AllocateMemory(state.device, &allocate, nullptr, &state.memory);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkAllocateMemory -> %s", result_name(result));
        return false;
    }
    result = pfn_BindBufferMemory(state.device, state.buffer, state.memory, 0);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkBindBufferMemory -> %s", result_name(result));
        return false;
    }
    result = pfn_MapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkMapMemory -> %s", result_name(result));
        return false;
    }
    line("vk_hello: buffer mapped at %p", state.mapped);
    return true;
}

bool
create_pipeline() {
    const VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(vk_hello_fill_spv),
        .pCode = vk_hello_fill_spv,
    };
    VkResult result = pfn_CreateShaderModule(state.device, &shader_info, nullptr, &state.shader);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateShaderModule -> %s", result_name(result));
        return false;
    }

    const VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    result = pfn_CreateDescriptorSetLayout(state.device, &set_layout_info, nullptr, &state.set_layout);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateDescriptorSetLayout -> %s", result_name(result));
        return false;
    }

    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &state.set_layout,
    };
    result = pfn_CreatePipelineLayout(state.device, &layout_info, nullptr, &state.pipeline_layout);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreatePipelineLayout -> %s", result_name(result));
        return false;
    }

    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = state.shader,
        .pName = "main",
    };
    const VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = state.pipeline_layout,
    };
    result = pfn_CreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                        &state.pipeline);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateComputePipelines -> %s", result_name(result));
        return false;
    }
    line("vk_hello: compute pipeline created");
    return true;
}

bool
create_commands_and_fence() {
    const VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    VkResult result = pfn_CreateDescriptorPool(state.device, &pool_info, nullptr, &state.descriptor_pool);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateDescriptorPool -> %s", result_name(result));
        return false;
    }
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = state.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &state.set_layout,
    };
    result = pfn_AllocateDescriptorSets(state.device, &allocate, &state.descriptor_set);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkAllocateDescriptorSets -> %s", result_name(result));
        return false;
    }
    const VkDescriptorBufferInfo buffer_info = {
        .buffer = state.buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    const VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = state.descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info,
    };
    pfn_UpdateDescriptorSets(state.device, 1, &write, 0, nullptr);

    const VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = state.queue_family,
    };
    result = pfn_CreateCommandPool(state.device, &command_pool_info, nullptr, &state.command_pool);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateCommandPool -> %s", result_name(result));
        return false;
    }
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = state.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = pfn_AllocateCommandBuffers(state.device, &command_info, &state.command_buffer);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkAllocateCommandBuffers -> %s", result_name(result));
        return false;
    }
    const VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    result = pfn_CreateFence(state.device, &fence_info, nullptr, &state.fence);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkCreateFence -> %s", result_name(result));
        return false;
    }
    return true;
}

bool
run_fill(uint32_t count) {
    auto *values = static_cast<uint32_t *>(state.mapped);
    for (uint32_t i = 0; i < count; i++)
        values[i] = 0xa5a5a5a5u;
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = state.memory,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    pfn_FlushMappedMemoryRanges(state.device, 1, &range);

    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult result = pfn_BeginCommandBuffer(state.command_buffer, &begin);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkBeginCommandBuffer -> %s", result_name(result));
        return false;
    }
    pfn_CmdBindPipeline(state.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
    pfn_CmdBindDescriptorSets(state.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline_layout,
                              0, 1, &state.descriptor_set, 0, nullptr);
    pfn_CmdDispatch(state.command_buffer, (count + 63) / 64, 1, 1);

    const VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    pfn_CmdPipelineBarrier(state.command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    result = pfn_EndCommandBuffer(state.command_buffer);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkEndCommandBuffer -> %s", result_name(result));
        return false;
    }

    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &state.command_buffer,
    };
    pfn_ResetFences(state.device, 1, &state.fence);
    // Conservative of a submit that fails half-way: the queue may have taken
    // part of the batch, so only a signalled fence may clear this.
    gpu_may_run = true;
    gpu_submitted = true;
    const double start = now_ms();
    result = pfn_QueueSubmit(state.queue, 1, &submit, state.fence);
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkQueueSubmit -> %s", result_name(result));
        return false;
    }
    result = pfn_WaitForFences(state.device, 1, &state.fence, VK_TRUE, UINT64_C(10000000000));
    const double dispatch_ms = now_ms() - start;
    if (result != VK_SUCCESS) {
        line("vk_hello: FAIL vkWaitForFences -> %s (10 s)", result_name(result));
        return false;
    }
    gpu_may_run = false;
    line("vk_hello: dispatch %u elements, %u workgroups, submit+wait %.2f ms", count, (count + 63) / 64,
         dispatch_ms);

    pfn_InvalidateMappedMemoryRanges(state.device, 1, &range);

    uint32_t bad = 0;
    uint32_t first_bad = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t expected = i * 3u + 7u;
        if (values[i] != expected) {
            if (bad == 0)
                first_bad = i;
            bad++;
        }
    }
    if (bad != 0) {
        line("vk_hello: FAIL fill mismatch: %u of %u elements, first at %u: got 0x%08x want 0x%08x", bad,
             count, first_bad, values[first_bad], first_bad * 3u + 7u);
        return false;
    }
    line("vk_hello: fill verified: %u/%u elements == i*3+7", count, count);
    line("vk_hello: timing dispatch+wait %.2f ms for %u elements", dispatch_ms, count);
    return true;
}

bool
run() {
    const double start = now_ms();

#ifdef PS5_TITLE
    // Before anything GPU: the pool RADV's winsys will report as its heap.
    log_pools();
#endif

    uint32_t sdk = 0;
    size_t len = sizeof(sdk);
    if (sysctlbyname("kern.sdk_version", &sdk, &len, nullptr, 0) == 0 && sdk)
        line("vk_hello: firmware %x.%02x (kern.sdk_version 0x%08x)", sdk >> 24, (sdk >> 16) & 0xff, sdk);
    else
        line("vk_hello: firmware unknown (kern.sdk_version unavailable)");

    // A payload has no title folder, so RADV's shader cache has nowhere to go.
    // A title has one: the winsys points MESA_SHADER_CACHE_DIR at
    // /app0/radv-shader-cache itself (ref/PS5_Mesa winsys), so leave it alone.
#ifndef PS5_TITLE
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
#endif
    // RADV splits an APU's pool into a 2/3 + 1/3 pair of heaps unless this is
    // set; PS5LM sets it before the first GPU call (ref/PS5LM, ps5/app/main.cpp)
    // so the device reports the whole direct-memory pool as one device-local
    // heap. Same call, same place: before the instance.
    setenv("radv_enable_unified_heap_on_apu", "true", 1);
    line("vk_hello: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)");
    // RADV (and ACO) report failures on stderr, which the loader drops; this
    // puts each line in klog with a "[radv]" prefix.
    line("vk_hello: driver stderr capture %s", ps5_klog_capture_stderr("[radv]") == 0 ? "on" : "off");

    const double t_instance = now_ms();
    if (!create_instance_and_device())
        return false;
    line("vk_hello: setup %.1f ms", now_ms() - t_instance);

    constexpr uint32_t kCount = 1u << 18;  // 262144 uints, 1 MiB
    if (!create_buffer_and_memory(kCount))
        return false;

    const double t_pipeline = now_ms();
    if (!create_pipeline())
        return false;
    line("vk_hello: pipeline %.1f ms (includes RADV's ACO compile)", now_ms() - t_pipeline);

    if (!create_commands_and_fence())
        return false;
    if (!run_fill(kCount))
        return false;

    line("vk_hello: total %.1f ms", now_ms() - start);
    return true;
}

}  // namespace

#ifdef PS5_TITLE
// app_crt's _start(process_parameters, loader_teardown) calls this as main().
// The title's argv is its own; vk_hello reads none.
int
main(int, char **) {
    line("vk_hello: start (ps5-splat-vr, title build, RADV linked directly, no loader)");
    // The home screen's splash would otherwise stay up over a headless probe
    // (docs/research/spike-b-memory.md section 6).
    sceSystemServiceHideSplashScreen();

    bool ok = false;
    if (run())
        ok = true;

    const double teardown = now_ms();
    destroy_all();
    line("vk_hello: teardown %.1f ms", now_ms() - teardown);
    line("vk_hello: %s", ok ? "PASS" : "FAIL");
    line("vk_hello: done, exit %d", ok ? 0 : 1);
    // A title may not return to the CRT nor exit(): the system ends it through
    // LoadExec("exit"), which is what PS5LM does.
    sceSystemServiceLoadExec("exit", nullptr);
    return ok ? 0 : 1;
}
#else
int
main() {
    line("vk_hello: start (ps5-splat-vr, RADV linked directly, no loader)");

    bool ok = false;
    if (run())
        ok = true;

    const double teardown = now_ms();
    destroy_all();
    line("vk_hello: teardown %.1f ms", now_ms() - teardown);
    line("vk_hello: %s", ok ? "PASS" : "FAIL");
    line("vk_hello: done, exit %d", ok ? 0 : 1);
    return ok ? 0 : 1;
}
#endif
