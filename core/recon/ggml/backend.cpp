#include "recon/ggml/backend.h"

#include <cstdio>
#include <utility>

#include "common/bytes.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#if SPLAT_HAVE_VULKAN
#include "ggml-vulkan.h"
#endif

namespace splat {

GgmlBackend::~GgmlBackend() {
    if (backend_ != nullptr) ggml_backend_free(backend_);
    backend_ = nullptr;
}

GgmlBackend::GgmlBackend(GgmlBackend&& other) noexcept
    : backend_(other.backend_), info_(std::move(other.info_)) {
    other.backend_ = nullptr;
}

GgmlBackend& GgmlBackend::operator=(GgmlBackend&& other) noexcept {
    if (this == &other) return *this;
    if (backend_ != nullptr) ggml_backend_free(backend_);
    backend_ = other.backend_;
    info_ = std::move(other.info_);
    other.backend_ = nullptr;
    return *this;
}

void GgmlBackend::refresh_memory() {
#if SPLAT_HAVE_VULKAN
    if (info_.kind != Kind::Vulkan) return;
    size_t free_memory = 0;
    size_t total_memory = 0;
    ggml_backend_vk_get_device_memory(0, &free_memory, &total_memory);
    info_.free_memory = free_memory;
    info_.total_memory = total_memory;
#endif
}

Result<GgmlBackend> GgmlBackend::create(bool prefer_vulkan, std::string* note) {
    GgmlBackend result;

#if SPLAT_HAVE_VULKAN
    if (prefer_vulkan) {
        const int devices = ggml_backend_vk_get_device_count();
        if (devices <= 0) {
            if (note != nullptr) *note = "no Vulkan device reported by the loader";
        } else {
            result.backend_ = ggml_backend_vk_init(0);
            if (result.backend_ == nullptr) {
                if (note != nullptr) *note = "Vulkan backend failed to initialize";
            } else {
                char description[256] = {};
                ggml_backend_vk_get_device_description(0, description, sizeof(description));
                size_t free_memory = 0;
                size_t total_memory = 0;
                ggml_backend_vk_get_device_memory(0, &free_memory, &total_memory);
                result.info_.kind = Kind::Vulkan;
                result.info_.name = description;
                result.info_.free_memory = free_memory;
                result.info_.total_memory = total_memory;
            }
        }
    }
#else
    if (prefer_vulkan && note != nullptr) *note = "built without the Vulkan backend";
#endif

    if (result.backend_ == nullptr) {
        result.backend_ = ggml_backend_cpu_init();
        if (result.backend_ == nullptr) return Result<GgmlBackend>::failure("cannot initialize the ggml CPU backend");
        result.info_.kind = Kind::Cpu;
        result.info_.name = "CPU";
    }
    return Result<GgmlBackend>::success(std::move(result));
}

ggml_backend_buffer_type_t GgmlBackend::buffer_type() const {
    return ggml_backend_get_default_buffer_type(backend_);
}

std::string GgmlBackend::describe() const {
    if (info_.kind == Kind::Vulkan) {
        return "Vulkan: " + info_.name + " (" + human_bytes(info_.total_memory) + " heap)";
    }
    return "CPU";
}

}  // namespace splat
