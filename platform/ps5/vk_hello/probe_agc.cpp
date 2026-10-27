// Spike B probe, step 0b: does a payload's loader resolve the three system
// modules that vk_hello's NEEDED list names?
//
// RADV's PS5 winsys and its WSI object import libSceAgc.sprx,
// libSceAgcDriver.sprx and libSceVideoOut.sprx as ordinary symbols (no
// dlopen), so the payload loader must resolve all three at load time. If it
// cannot, vk_hello never starts; this payload answers the same question in
// seconds instead of the 97 MiB deploy, and its own start line is half the
// answer.
//
// It imports every entry point RADV needs, prints each address (non-zero means
// the loader bound the import), and calls only the two AGC calls that are safe
// without GPU state. It submits no work and does not open the display: taking
// VideoOut's handle from the home screen would be a change this probe has no
// reason to make.
//
// Build: make -C platform/ps5/vk_hello probe_agc.elf
// Run:   platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_agc.elf 6
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <ps5/klog.h>

// libSceAgc, as the platform layer declares them (PS5_PayloadSDK,
// include/ps5platform/agc.h) and as RADV's PS5 winsys calls them.
extern "C" int32_t sceAgcInit(uint32_t version);
extern "C" int32_t sceAgcSuspendPoint(void);

// libSceAgcDriver: RADV's submission path (PS5_Mesa
// src/amd/vulkan/winsys/ps5). Addressed, never called: submitting a DCB or
// setting the tessellation rings needs a live device behind it.
extern "C" int32_t sceAgcDriverSubmitDcb(const void *description);
extern "C" int32_t sceAgcDriverSetTFRing(uint64_t address, uint32_t size);
extern "C" int32_t sceAgcDriverSetHsOffchipParam(uint32_t granularity, uint32_t buffering);

// libSceVideoOut: the WSI functions RADV's archive carries (the SDK's
// ps5platform/videoout.h). Addressed, never called, for the reason above.
extern "C" int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern "C" int32_t sceVideoOutClose(int32_t handle);
extern "C" int32_t sceVideoOutGetFlipStatus(int32_t handle, uint64_t *status);

static void
line(const char *fmt, ...) {
    char    msg[384];
    va_list args;

    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    std::printf("%s\n", msg);
    std::fflush(stdout);
    klog_puts(msg);
}

// An imported entry point's address. In a PIE this is the bound slot; it is
// non-zero once the loader resolved the symbol from its module.
#define ADDRESS(fn) reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(&fn))

struct Import {
    const char *module;
    const char *name;
    const void *address;
};

int
main() {
    line("vkhello-agc: start (ps5-splat-vr, linked AGC, AGC driver and VideoOut imports)");

    const Import imports[] = {
        {"libSceAgc.sprx", "sceAgcInit", ADDRESS(sceAgcInit)},
        {"libSceAgc.sprx", "sceAgcSuspendPoint", ADDRESS(sceAgcSuspendPoint)},
        {"libSceAgcDriver.sprx", "sceAgcDriverSubmitDcb", ADDRESS(sceAgcDriverSubmitDcb)},
        {"libSceAgcDriver.sprx", "sceAgcDriverSetTFRing", ADDRESS(sceAgcDriverSetTFRing)},
        {"libSceAgcDriver.sprx", "sceAgcDriverSetHsOffchipParam", ADDRESS(sceAgcDriverSetHsOffchipParam)},
        {"libSceVideoOut.sprx", "sceVideoOutOpen", ADDRESS(sceVideoOutOpen)},
        {"libSceVideoOut.sprx", "sceVideoOutClose", ADDRESS(sceVideoOutClose)},
        {"libSceVideoOut.sprx", "sceVideoOutGetFlipStatus", ADDRESS(sceVideoOutGetFlipStatus)},
    };

    unsigned resolved = 0;
    for (const Import &import : imports) {
        line("vkhello-agc: import %s %s @ %p%s", import.module, import.name, import.address,
             import.address ? "" : " (unresolved)");
        if (import.address)
            resolved++;
    }
    line("vkhello-agc: imports resolved %u/%u", resolved, (unsigned)(sizeof(imports) / sizeof(imports[0])));

    int32_t init = sceAgcInit(8);
    line("vkhello-agc: sceAgcInit(8) -> 0x%08x", (uint32_t)init);
    line("vkhello-agc: sceAgcSuspendPoint() -> 0x%08x", (uint32_t)sceAgcSuspendPoint());

    line("vkhello-agc: %s", resolved == sizeof(imports) / sizeof(imports[0]) ? "PASS" : "FAIL");
    line("vkhello-agc: done");
    return 0;
}
