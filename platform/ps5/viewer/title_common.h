// What the viewer title's translation units share: the klog line, the millisecond
// clock and the big-stack call. The TV path (main.cpp) and the VR path
// (vr_mode.cpp) are one program; these three are the parts both need, so they
// live here instead of being copied into the second file. The two Vulkan
// helpers every title (this one and vr_hello) needs are in vk_common.h.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <pthread.h>

#include <ps5platform/klog.h>

#include <vulkan/vulkan.h>

namespace viewer {

// ---- logging ---------------------------------------------------------------
// A title has no stdout reader: klog is the one live sink (docs/research/
// spike-b-memory.md section 5). Every line is `viewer: ...`, so the runbook's
// grep is one word; the kernel prefixes the process name (the title id).
inline void line(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline void line(const char* format, ...) {
    char message[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    char out[800];
    std::snprintf(out, sizeof(out), "%s\n", message);
    sceKernelDebugOutText(0, out);
}

// CLOCK_MONOTONIC in milliseconds. The VR path also reads hmd2's own clock
// (sceKernelGetProcessTime) for the frame cadence; this one is for the logs and
// for load timings.
inline double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

// ---- big-stack calls -------------------------------------------------------
// ACO recurses deeply (NIR passes, register allocation, the scheduler) and a
// title's main stack is small; a pipeline compile that overflows it dies with
// no log line. Every renderer creation therefore runs on a 32 MiB stack thread
// we allocate, as PS5_Vulkan's diagnostics do for its compiles. If the console
// refuses the stack, the call runs where it was made and says so.
struct StackCall {
    void (*run)(void*);
    void* argument;
};

inline void* stack_entry(void* argument) {
    StackCall* call = static_cast<StackCall*>(argument);
    call->run(call->argument);
    return nullptr;
}

template <typename Fn>
void run_with_big_stack(Fn& call, const char* what) {
    constexpr size_t kStackBytes = 32u * 1024u * 1024u;
    StackCall wrapper{[](void* argument) { (*static_cast<Fn*>(argument))(); }, &call};
    pthread_attr_t attributes;
    pthread_t thread;
    char* stack = static_cast<char*>(std::malloc(kStackBytes));
    if (pthread_attr_init(&attributes) != 0) {
        std::free(stack);
        call();
        return;
    }
    int created = -1;
    if (stack != nullptr) created = pthread_attr_setstack(&attributes, stack, kStackBytes);
    if (created != 0) created = pthread_attr_setstacksize(&attributes, kStackBytes);
    if (created == 0) created = pthread_create(&thread, &attributes, stack_entry, &wrapper);
    pthread_attr_destroy(&attributes);
    if (created == 0) {
        pthread_join(thread, nullptr);
        std::free(stack);
        return;
    }
    std::free(stack);
    line("viewer: %s: a 32 MiB stack thread was refused (%d); running on the main stack", what,
         created);
    call();
}

}  // namespace viewer
