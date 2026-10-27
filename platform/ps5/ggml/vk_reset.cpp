// Release the VkDevice ggml-vulkan caches for a physical device index.
//
// op_audit measures every case once at the bench.env coopmat setting and once
// with GGML_VK_DISABLE_COOPMAT flipped, in the same process (the console run
// has no way to pass an argument or a second launch to a payload). Cooperative
// matrices are a device-creation feature: ggml-vulkan reads the variable in
// ggml_vk_get_device() while it builds the vk_device_struct and then keeps it
// in vk_instance.devices[idx] for the life of the process. Freeing the backend
// does not drop that cache, so the setting cannot be changed between passes
// without releasing the device first.
//
// This is the one place that reaches into ggml-vulkan's internals. It is a
// payload-only helper, not a core/ dependency and not a ggml patch: the same
// vk_instance_t definition from the ggml tree is included, so the layout is
// the compiler's, not a copy. The next ggml_vk_get_device(0) call (from
// ggml_backend_dev_init) then builds a fresh device with the current
// environment. Nothing here runs while a submission is in flight: op_audit
// calls it only after a pass has read every result back and freed every
// buffer.
//
// Compiled into libsplat_ps5_glue.a (platform/ps5/ggml/CMakeLists.txt).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bench_env.h"

#include "ggml-vulkan-types.h"

extern vk_instance_t vk_instance;

namespace splat {
namespace bench {

bool vk_reset_available() { return true; }

int vk_device_coopmat(int idx) {
    if (idx < 0 || idx >= GGML_VK_MAX_DEVICES || !vk_instance.devices[idx]) return -1;
    return vk_instance.devices[idx]->coopmat_support ? 1 : 0;
}

bool vk_release_device(int idx) {
    if (idx < 0 || idx >= GGML_VK_MAX_DEVICES || !vk_instance.devices[idx]) return false;
    vk_device device = vk_instance.devices[idx];

    // The device owns two strong references back to itself, so dropping the
    // cache entry alone would leave it (and its VkDevice and pipelines)
    // unreachable but alive:
    //   device->sync_staging            : vk_buffer_struct holds a vk_device
    //   device->buffer_type.context     : {name, vk_device}, raw pointer owned
    //                                     by the device
    // Break both, then drop the cache entry; the destructor then runs and
    // destroys the fences, queues, buffers, pipelines and the VkDevice. Every
    // buffer from the pass is already freed and the GPU is idle here (op_audit
    // read every result back and freed the backend), so nothing is used after.
    if (device->sync_staging) {
        device->sync_staging.reset();
    }
    if (device->buffer_type.context != nullptr) {
        auto* context =
            static_cast<ggml_backend_vk_buffer_type_context*>(device->buffer_type.context);
        device->buffer_type.context = nullptr;
        delete context;
    }
    vk_instance.devices[idx].reset();
    return true;
}

}  // namespace bench
}  // namespace splat
