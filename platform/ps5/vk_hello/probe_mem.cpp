// Spike B probe, step 0: what a payload process can give RadV.
// Measures the direct-memory pool (the pool the PS5 RADV winsys allocates its
// buffers from), the flexible pool, and whether AGC can be reached by dlopen.
// Every line goes to stdout and to the kernel log.
//
// Build: make -C platform/ps5/vk_hello probe_mem.elf
// Run:   platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_mem.elf 8
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <sys/types.h>

#include <ps5/klog.h>

extern "C" size_t sceKernelGetDirectMemorySize(void);
extern "C" int    sceKernelAvailableDirectMemorySize(off_t search_start, off_t search_end,
                                                     size_t alignment, off_t *phys_out,
                                                     size_t *size_out);
extern "C" int    sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len,
                                                size_t alignment, int memory_type, off_t *phys_out);
extern "C" int    sceKernelReleaseDirectMemory(off_t start, size_t len);
extern "C" int    sceKernelAvailableFlexibleMemorySize(size_t *size_out);

using AgcInit = int32_t (*)(uint32_t);
using AgcSuspendPoint = int32_t (*)(void);

static void
line(const char *fmt, ...) {
    char    msg[512];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

static const char *
hex32(uint32_t value, char *buf) {
    std::snprintf(buf, 16, "0x%08x", value);
    return buf;
}

int
main() {
    char code[16];

    line("vkhello-probe: start (ps5-splat-vr)");

    // --- direct memory: what the RADV PS5 winsys allocates from -------------
    const size_t total = sceKernelGetDirectMemorySize();
    line("vkhello-probe: direct memory size %zu bytes (%.2f GiB)", total,
         (double)total / (1024.0 * 1024.0 * 1024.0));

    size_t free_bytes = 0;
    off_t  phys = 0;
    int    rv = sceKernelAvailableDirectMemorySize(0, (off_t)total, 2 * 1024 * 1024, &phys,
                                                  &free_bytes);
    line("vkhello-probe: available direct memory rv=%s free=%zu bytes", hex32((uint32_t)rv, code),
         free_bytes);

    const size_t request = 64u * 1024u * 1024u;
    off_t        start = 0;
    int          types[] = {12, 0};
    for (int type : types) {
        rv = sceKernelAllocateDirectMemory(0, (off_t)total, request, 2 * 1024 * 1024, type, &start);
        line("vkhello-probe: allocate direct 64 MiB type %d rv=%s phys=%lld", type,
             hex32((uint32_t)rv, code), (long long)start);
        if (rv == 0) {
            sceKernelReleaseDirectMemory(start, request);
            break;
        }
    }

    // --- flexible pool: libc's heap in a payload ---------------------------
    size_t flexible = 0;
    rv = sceKernelAvailableFlexibleMemorySize(&flexible);
    line("vkhello-probe: available flexible memory rv=%s free=%zu bytes (%.2f GiB)",
         hex32((uint32_t)rv, code), flexible, (double)flexible / (1024.0 * 1024.0 * 1024.0));

    // --- can a payload dlopen AGC? ----------------------------------------
    const char *names[] = {"libSceAgc.sprx", "libSceAgcDriver.sprx", "libSceAgc.prx", nullptr};
    for (int i = 0; names[i]; i++) {
        void *handle = dlopen(names[i], RTLD_NOW);
        const char *err = handle ? "" : (dlerror() ? " (dlerror set)" : " (no dlerror)");
        line("vkhello-probe: dlopen %s -> %s%s", names[i], handle ? "ok" : "NULL", err);
        if (!handle)
            continue;
        AgcInit         init = (AgcInit)dlsym(handle, "sceAgcInit");
        AgcSuspendPoint suspend = (AgcSuspendPoint)dlsym(handle, "sceAgcSuspendPoint");
        line("vkhello-probe:   dlsym sceAgcInit=%p sceAgcSuspendPoint=%p", (void *)init,
             (void *)suspend);
        if (init)
            line("vkhello-probe:   sceAgcInit(8) -> %s", hex32((uint32_t)init(8), code));
        if (suspend)
            line("vkhello-probe:   sceAgcSuspendPoint() -> %s",
                 hex32((uint32_t)suspend(), code));
        dlclose(handle);
    }

    line("vkhello-probe: done");
    return 0;
}
