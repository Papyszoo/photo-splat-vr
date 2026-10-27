// The handful of Vulkan/libc entry points a loader-less ggml-vulkan payload
// still calls by name.
//
// RADV's archive (libvulkan_radeon.ps5.a) exports vk_icdGetInstanceProcAddr,
// the ICD entry a Vulkan loader calls; it does not export vkGetInstanceProcAddr
// or the three functions below, because without a loader nothing is supposed
// to reference them by name. ggml-vulkan does, in four places:
//
//   vkGetInstanceProcAddr        vulkan-hpp DispatchLoaderDynamic init
//                                (ggml_vulkan.cpp, ggml_vk_instance_init)
//   vkGetPhysicalDeviceFeatures2 ggml_vk_get_device: feature struct chain
//   vkGetDeviceProcAddr          ggml_vk_get_device: VK_EXT_device_fault hook
//   vkCmdCopyBuffer              ggml_vk_buffer_copy_async
//
// Each shim resolves its own pointer through RADV's ICD entry with the
// instance the dispatcher was created for, and forwards. That is the same
// lookup a loader does; there is no layer, no dispatch table, no allocation.
// The instance is captured from the first non-null vkGetInstanceProcAddr call,
// which vulkan-hpp makes before any of these can run.
//
// vk_hello reaches the ICD directly; this file is the only difference between
// "a program that talks to RADV" and "a ggml-vulkan program that does".
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cerrno>
#include <cstdarg>
#include <sys/types.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char * name);

namespace {

VkInstance g_instance = VK_NULL_HANDLE;

template <typename Fn>
Fn icd(const char * name) {
    return reinterpret_cast<Fn>(vk_icdGetInstanceProcAddr(g_instance, name));
}

}  // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    if (instance != VK_NULL_HANDLE) g_instance = instance;
    return vk_icdGetInstanceProcAddr(instance, name);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physical_device, VkPhysicalDeviceFeatures2 * features) {
    static PFN_vkGetPhysicalDeviceFeatures2 fn = nullptr;
    if (fn == nullptr) fn = icd<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    fn(physical_device, features);
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char * name) {
    static PFN_vkGetDeviceProcAddr fn = nullptr;
    if (fn == nullptr) fn = icd<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    return fn(device, name);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer(VkCommandBuffer command_buffer, VkBuffer src, VkBuffer dst, uint32_t region_count,
                const VkBufferCopy * regions) {
    static PFN_vkCmdCopyBuffer fn = nullptr;
    if (fn == nullptr) fn = icd<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    fn(command_buffer, src, dst, region_count, regions);
}

// ---- libc gaps -------------------------------------------------------------
//
// ggml's crash handler forks gdb/lldb (ggml.c, ggml_print_backtrace); a
// payload cannot and should not. fork() failing is exactly the "no debugger
// available" path it already handles.
extern "C" pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

extern "C" int execlp(const char * file, const char * arg, ...) {
    (void) file;
    (void) arg;
    errno = ENOSYS;
    return -1;
}

// libc++'s std::filesystem::current_path() asks pathconf(_PC_PATH_MAX) for its
// buffer; the PS5 libc does not export pathconf. -1 makes libc++ use its own
// default, which is what every call site in these payloads wants.
extern "C" long pathconf(const char * path, int name) {
    (void) path;
    (void) name;
    errno = ENOSYS;
    return -1;
}
