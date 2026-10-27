#pragma once

#include <cstdint>
#include <string>

#include "common/status.h"
#include "ggml-backend.h"

namespace splat {

// C3/C4 shared ggml backend: Vulkan when it initializes, CPU otherwise.
//
// The console target always has Vulkan; the desktop build uses MoltenVK when a
// loader and an ICD are present (macOS) and falls back to the CPU backend so
// tests and headless runs still work. Everything upstream (weights, graph
// splits) only ever sees ggml_backend_t, so the choice is a one-line policy.
class GgmlBackend {
public:
    enum class Kind { Cpu, Vulkan };

    struct Info {
        Kind kind = Kind::Cpu;
        std::string name;          // "CPU" or the Vulkan device description
        uint64_t total_memory = 0; // device heap, 0 when unknown
        uint64_t free_memory = 0;
    };

    GgmlBackend() = default;
    ~GgmlBackend();

    GgmlBackend(const GgmlBackend&) = delete;
    GgmlBackend& operator=(const GgmlBackend&) = delete;
    GgmlBackend(GgmlBackend&& other) noexcept;
    GgmlBackend& operator=(GgmlBackend&& other) noexcept;

    // prefer_vulkan=false forces CPU, which is what the parity test uses to
    // keep CI (and this Mac, when a run must be bit-stable) on one path.
    // `note` receives why Vulkan was not used, when it was requested.
    static Result<GgmlBackend> create(bool prefer_vulkan, std::string* note = nullptr);

    ggml_backend_t handle() const { return backend_; }
    ggml_backend_buffer_type_t buffer_type() const;
    const Info& info() const { return info_; }
    bool ok() const { return backend_ != nullptr; }

    // Re-reads the device heap into info().free_memory / total_memory. Vulkan
    // only (the CPU backend has no heap budget), so a load/unload test can see
    // what the backend gave back. Cheap: one vkGetPhysicalDeviceMemoryProperties2.
    void refresh_memory();

    // Formats bytes as "1.23 GiB" for logs.
    std::string describe() const;

private:
    ggml_backend_t backend_ = nullptr;
    Info info_;
};

}  // namespace splat
