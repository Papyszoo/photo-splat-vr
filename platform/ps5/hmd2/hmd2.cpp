// hmd2 - PS VR2 output for a PS5 payload. See hmd2.h.
//
// Ported from PS5VR (GPL-3.0-or-later, Copyright (C) 2026 Husam Osman):
// app/src/app_vr.c for the module loading, the tracker/reprojection setup, the
// per-frame layer and the shutdown order, and app/engine/media/src/
// eng_agc_writer.c for the texture descriptors. The tiled-address formula is
// from PS5_Vulkan's CPU renderer (src/demo_renderer.cpp). The predicted-time
// pacing, the vblank gate after EnableVrMode, the re-submit of begun-but-
// unsubmitted frames and the linear RGBA8 eye descriptor are HLA-PSVR2's
// (GPL-3.0-or-later, Copyright (C) 2026 theghostonline; ps5xr/runtime/src/
// psvr2.c and xr_session.c). See THIRD_PARTY_NOTICES.md.
//
// The eye buffers here are plain direct memory filled from the CPU, not AGC
// render targets: that is the difference from PS5VR and the reason a payload
// can use this without linking AGC or RADV.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hmd2.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>

// System functions, declared here as PS5VR declares them: the payload SDK has
// stubs for all of them but no headers.
extern "C" {
int sceSysmoduleLoadModule(uint32_t id);
const char *sceKernelGetFsSandboxRandomWord(void);
int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags,
                             void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceKernelGetModuleInfo(int handle, void *info);
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                  int64_t *offset);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                             size_t align);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelUsleep(unsigned usec);
uint64_t sceKernelGetProcessTime(void);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetLoginUserIdList(int user_ids[4]);
int sceUserServiceGetInitialUser(int *user_id);

// libSceVideoOut. On bus type 32 the port is Hmd2's "VR driver".
struct Hmd2VideoOutBuffer {
    void *data, *metadata, *reserved0, *reserved1;
};
int sceVideoOutOpen(int user_id, int bus_type, int index, const void *param);
int sceVideoOutClose(int handle);
void sceVideoOutSetBufferAttribute2(void *attr, uint64_t format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t option, uint32_t reserved0,
                                    uint64_t reserved1);
int sceVideoOutRegisterBuffers2(int handle, int set_index, int start_index, void *buffers,
                                int count, void *attr, int category, void *option);
int sceVideoOutUnregisterBuffers(int handle, int set_index);
int sceVideoOutSubmitFlip(int handle, int index, int flip_mode, int64_t flip_arg);
int sceVideoOutConfigureOutput(int handle, uint64_t mode, const void *options, const void *r0,
                               uint64_t r1);
int sceVideoOutGetFlipStatus(int handle, uint64_t status[16]);
int sceVideoOutIsFlipPending(int handle);
int sceVideoOutGetVblankStatus(int handle, void *status);
}

namespace hmd2 {
namespace {

// -- constants --------------------------------------------------------------
constexpr uint32_t kSysmoduleHmd2 = 0x12a;
constexpr uint32_t kSysmoduleVrTracker2 = 0x12b;
// The output mode and VR mode PS5VR asks for: 120 Hz with half-rate rendering,
// so every submitted frame is shown for exactly two refreshes.
constexpr uint64_t kVrMode120Hz = 0x2000f;
constexpr uint64_t kFrameUs = 16667;       // 60 Hz frame cadence
// HLA-PSVR2's pacing constants: the flip latency it measured at 120 Hz output
// and the margin before the predicted display time (xr_session.c:601-621).
constexpr uint64_t kLatencyUs = 4160;
constexpr uint64_t kLeadUs = kFrameUs + kLatencyUs + 1500;
constexpr int kPendingFrames = 4;
constexpr int kMaxPendingQueueFull = 60;
constexpr int kVrQueueFull = (int)0x8A72001A;
constexpr int kVideoOutBusy = (int)0x80290009;
constexpr uint64_t kVideoOutFormatB8G8R8A8 = 0x8000000000000000ull;
constexpr uint32_t kAttrBytes = 80;
constexpr uint32_t kLayerBytes = 0x130;
constexpr uint32_t kParamLayerBytes = 0x148;
constexpr int kMaxBuffers = 3;

struct SizeAlign {
    uint64_t size, align;
};
struct LayerSet {
    void *layers;
    uint32_t count;
    uint32_t pad;
};

// -- Hmd2 and VrTracker2, resolved at runtime -------------------------------
#define HMD2_FUNCS(X)                                                                           \
    X(int, sceHmd2Initialize, (const void *param))                                              \
    X(int, sceHmd2Terminate, (void))                                                            \
    X(int, sceHmd2Open, (int user_id, int type, int index, const void *param))                  \
    X(int, sceHmd2Close, (int handle))                                                          \
    X(int, sceHmd2GetDeviceInformation, (void *info))                                           \
    X(int, sceHmd2GetFieldOfViewWithoutHandle, (void *fov))                                     \
    X(SizeAlign, sceHmd2ReprojectionQueryBufferSizeAlign, (void))                                \
    X(SizeAlign, sceHmd2ReprojectionQueryDisplayBufferSizeAlign, (void))                         \
    X(int, sceHmd2ReprojectionInitialize, (const void *param, const void *reserved))            \
    X(int, sceHmd2ReprojectionEnableVrMode, (uint64_t mode))                                    \
    X(int, sceHmd2ReprojectionDisableVrMode, (void))                                            \
    X(int, sceHmd2ReprojectionBeginFrame, (uint64_t frame))                                     \
    X(int, sceHmd2ReprojectionGetPredictedDisplayTime, (uint64_t frame, uint64_t *time))        \
    X(int, sceHmd2ReprojectionSetParam, (uint32_t buffer, uint64_t frame, const void *param,     \
                                         const void *trace))                                   \
    X(int, sceHmd2ReprojectionSetRenderConfig, (uint64_t half_rate, uint64_t r0, uint64_t r1))  \
    X(int, sceHmd2ReprojectionTerminate, (void))
// The TV mirror's five exports are optional: a library without them costs the
// TV, not the VR session, so they are resolved (only when Config::mirror asks
// for them) apart from the required slots above.
#define HMD2_MIRROR_FUNCS(X)                                                                    \
    X(SizeAlign, sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign, (void))                     \
    X(SizeAlign, sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign, (void))                  \
    X(int, sceHmd2ReprojectionSetMirroringOption, (const void *option))                          \
    X(int, sceHmd2ReprojectionEnableMirroring, (const void *param))                              \
    X(int, sceHmd2ReprojectionDisableMirroring, (void))
#define VRTRACKER2_FUNCS(X)                                                                     \
    X(int, sceVrTracker2QueryMemory, (const void *param, void *out))                            \
    X(int, sceVrTracker2Initialize, (const void *param))                                        \
    X(int, sceVrTracker2Finalize, (void))                                                       \
    X(int, sceVrTracker2RegisterDevice, (int type, int handle))                                 \
    X(int, sceVrTracker2UnregisterDevice, (int handle))                                         \
    X(int, sceVrTracker2GetResult, (const void *param, void *result))
#define VRHAND_FUNCS(X)                                                                         \
    X(int, sceVrHandInitialize, (const void *param))                                            \
    X(int, sceVrHandFinalize, (void))                                                           \
    X(int, sceVrHandStart, (void))                                                              \
    X(int, sceVrHandStop, (void))                                                               \
    X(int, sceVrHandGetResult, (const void *param, void *result))

#define DECLARE_PTR(ret, name, args) ret (*name) args = nullptr;
HMD2_FUNCS(DECLARE_PTR)
HMD2_MIRROR_FUNCS(DECLARE_PTR)
VRTRACKER2_FUNCS(DECLARE_PTR)
VRHAND_FUNCS(DECLARE_PTR)
#undef DECLARE_PTR

struct Slot {
    const char *name;
    void **slot;
};
#define SLOT(ret, name, args) {#name, reinterpret_cast<void **>(&name)},
const Slot kHmd2Slots[] = {HMD2_FUNCS(SLOT)};
const Slot kHmd2MirrorSlots[] = {HMD2_MIRROR_FUNCS(SLOT)};
const Slot kVrTracker2Slots[] = {VRTRACKER2_FUNCS(SLOT)};
const Slot kVrHandSlots[] = {VRHAND_FUNCS(SLOT)};
#undef SLOT

template <size_t N>
constexpr size_t kSlots(const Slot (&)[N]) {
    return N;
}

// -- export tables ----------------------------------------------------------
// The tables hmd2_exports.h holds, generated from tools/export-tables.
struct export_sig_t {
    const char *name;
    uint32_t offset;
};
struct export_anchor_t {
    uint32_t va;
    const char *text;
};
struct export_table_t {
    int fw;
    const export_sig_t *exports;
    size_t count;
    const export_anchor_t *anchor;
};
#include "hmd2_exports.h"

// -- a loaded library's module info -----------------------------------------
// sceKernelGetModuleInfo's 0x160-byte record: size, name, up to four segments,
// and a 20-byte fingerprint.
struct ModuleSegment {
    uint64_t addr;
    uint32_t size;
    int32_t prot;
};
struct ModuleInfo {
    uint64_t size;
    char name[256];
    ModuleSegment seg[4];
    uint32_t nseg;
    uint8_t fingerprint[20];
};

// -- state ------------------------------------------------------------------
struct State {
    Logger log = nullptr;
    bool inited = false;
    bool failed = false;   // last init() attempt failed; informational, not a latch
    bool active = false;
    bool hmd2_up = false;      // sceHmd2Initialize succeeded
    bool tracker_inited = false;  // sceVrTracker2Initialize succeeded
    bool tracker_up = false;   // sceVrTracker2RegisterDevice succeeded (needs inited)
    bool reproj_up = false;    // sceHmd2ReprojectionInitialize succeeded
    bool vr_mode = false;      // sceHmd2ReprojectionEnableVrMode succeeded
    bool registered = false;   // sceVideoOutRegisterBuffers2 succeeded
    int user = 0xff;
    int hmd = -1;
    int vh = -1;
    // Sense controller pad handles registered with VrTracker2, one per hand
    // (slot 0 right, slot 1 left; -1 = none). Unregistered in teardown().
    int controllers[2] = {-1, -1};
    int fw = 0;               // the export table that bound the modules, 0 for dlsym
    float fov[4] = {0, 0, 0, 0};
    // Per-eye size. PSVR2's is not square: 1984 wide (the panel's 2000 pixels
    // rounded down to whole 64-texel blocks) by 2048 high, so the side-by-side
    // image is 3968x2048. The library's own buffers are square (Config).
    uint32_t eye_w = 1024;
    uint32_t eye_h = 1024;
    int buffers = kMaxBuffers;
    size_t image_bytes = 0;   // one side-by-side image
    void *eye_mem = nullptr;
    size_t eye_mem_bytes = 0;
    int64_t eye_mem_off = -1;
    void *tracker_mem = nullptr;
    size_t tracker_mem_bytes = 0;
    int64_t tracker_mem_off = -1;
    void *reproj_work = nullptr;
    void *reproj_display = nullptr;
    size_t reproj_work_bytes = 0;
    size_t reproj_display_bytes = 0;
    int64_t reproj_work_off = -1;
    int64_t reproj_display_off = -1;
    // The TV mirror (Config::mirror): Hmd2's work and display memory, and
    // whether the module took them. `_bytes` is what direct_alloc mapped (the
    // free needs it); `mirror_display_query` is Hmd2's own size, which is the
    // only thing it says about the mirror's geometry.
    bool mirror = false;
    bool mirror_on = false;
    void *mirror_work = nullptr;
    void *mirror_display = nullptr;
    size_t mirror_work_bytes = 0;
    size_t mirror_display_bytes = 0;
    size_t mirror_display_query = 0;
    int64_t mirror_work_off = -1;
    int64_t mirror_display_off = -1;
    // External eye images (Config::external), copied out of the config so the
    // Config may be a temporary.
    bool external = false;
    void *ext_address[kMaxBuffers] = {};
    size_t ext_bytes[kMaxBuffers] = {};
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    alignas(16) uint8_t layers[kMaxBuffers][kLayerBytes] = {};
    LayerSet sets[kMaxBuffers] = {};
    int buf = 0;
    uint64_t frame = 0;
    uint64_t display_time = 0;
    Pose pose = {{0, 0, 0}, {0, 0, 0, 1}, false};
    // Per-eye poses from the same tracker result; untracked until one carries
    // two plausible eye points (see hmd2.h: Poses).
    Pose eye[2] = {{{0, 0, 0}, {0, 0, 0, 1}, false}, {{0, 0, 0}, {0, 0, 0, 1}, false}};
    // The last tracked sample, handed back (untracked) when the tracker drops
    // out: the submitted pose must stay the pose the image was drawn from.
    Pose held = {{0, 0, 0}, {0, 0, 0, 1}, false};
    Pose held_eye[2] = {{{0, 0, 0}, {0, 0, 0, 1}, false}, {{0, 0, 0}, {0, 0, 0, 1}, false}};
    bool frame_open = false;
    int submits = 0;
    int errors = 0;
    uint64_t worn_checked = 0;
    DeviceState device = {0, true, 0};
    Error error = {"", 0};

    // Pacing (HLA-PSVR2's rules): the previous BeginFrame and its predicted
    // display, the caller's measured work, and the frames Hmd2 has opened but
    // not had an image for.
    uint64_t last_begin_us = 0;
    uint64_t last_display_us = 0;
    uint64_t last_work_us = 0;
    uint64_t frame_started_us = 0;
    bool live_logged = false;
    struct PendingFrame {
        uint64_t frame;
        uint64_t display;
    } pending[kPendingFrames] = {};
    int npending = 0;
    bool have_last = false;
    uint32_t last_buf = 0;
    Pose last_pose = {{0, 0, 0}, {0, 0, 0, 1}, false};
    int repeated = 0;
    int queue_full = 0;
    int missed = 0;
    uint64_t pace_nap = 0, pace_call = 0, pace_ahead = 0, pace_step = 0, pace_n = 0;
};

State s;

void logf(const char *fmt, ...) {
    if (s.log == nullptr)
        return;
    char msg[384];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    s.log(msg);
}

bool fail(const char *step, int32_t rc) {
    s.error.step = step;
    s.error.rc = rc;
    logf("hmd2: %s -> 0x%08x (failed)", step, (unsigned)rc);
    return false;
}

// -- direct memory ----------------------------------------------------------
// WB_ONION, laid out the way PS5VR and PS5_Vulkan do it: the mapping's address
// is also the address the GPU samples through.
void *direct_alloc(size_t size, size_t align, const char *what, int64_t *off_out, size_t *bytes_out) {
    size = (size + 0xffff) & ~(size_t)0xffff;
    int64_t off = -1;
    void *va = nullptr;
    int rc = sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, size, align, 12 /* WB_ONION */, &off);
    if (rc == 0)
        rc = sceKernelMapDirectMemory(&va, size, 0x33 /* CPU+GPU RW */, 0, off, align);
    if (rc != 0 || va == nullptr) {
        if (off >= 0)
            sceKernelReleaseDirectMemory(off, size);
        logf("hmd2: direct memory %s (0x%zx bytes) -> 0x%08x", what, size, (unsigned)rc);
        return nullptr;
    }
    std::memset(va, 0, size);
    *off_out = off;
    *bytes_out = size;
    logf("hmd2: direct memory %s: 0x%zx bytes at %p (offset 0x%llx)", what, size, va,
         (unsigned long long)off);
    return va;
}

void direct_free(void **va, size_t bytes, int64_t off) {
    if (*va != nullptr)
        sceKernelMunmap(*va, bytes);
    if (off >= 0)
        sceKernelReleaseDirectMemory(off, bytes);
    *va = nullptr;
}

// -- cache -----------------------------------------------------------------
// The GPU and the display controller read direct memory straight from DRAM, so
// CPU writes have to be written back before Hmd2 samples them: the same
// clflush-per-line + mfence PS5VR's eng_agc_runtime_cache_flush and
// PS5_Vulkan's ps5vk_flush_cpu_cache do.
void flush_cpu_writes(const void *address, size_t bytes) {
#if defined(__x86_64__) || defined(__amd64__)
    const uintptr_t first = reinterpret_cast<uintptr_t>(address) & ~static_cast<uintptr_t>(63);
    const uintptr_t end = reinterpret_cast<uintptr_t>(address) + bytes;
    for (uintptr_t at = first; at < end; at += 64)
        __asm__ volatile("clflush (%0)" ::"r"(reinterpret_cast<const void *>(at)) : "memory");
    __asm__ volatile("mfence" ::: "memory");
#else
    (void)address;
    (void)bytes;
#endif
}

// -- module loading ---------------------------------------------------------
// id 0: no sysmodule to load, only the library path (libSceVrHand).
int load_sysmodule(uint32_t id, const char *name) {
    const int rc = id != 0 ? sceSysmoduleLoadModule(id) : 0;
    char path[160];
    std::snprintf(path, sizeof path, "/%s/common/lib/%s.sprx", sceKernelGetFsSandboxRandomWord(), name);
    int res = 0;
    const int handle = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &res);
    if (id != 0)
        logf("hmd2: %s: sysmodule 0x%x -> 0x%08x", name, id, (unsigned)rc);
    logf("hmd2: %s: load \"%s\" -> handle 0x%x (res 0x%08x)", name, path, handle,
         (unsigned)res);
    return handle;
}

uintptr_t module_base(int handle, const char *file, ModuleInfo *info) {
    std::memset(info, 0, sizeof *info);
    info->size = sizeof *info;
    if (sceKernelGetModuleInfo(handle, info) != 0 || std::strcmp(info->name, file) != 0)
        return 0;
    uintptr_t base = 0;
    for (uint32_t i = 0; i < info->nseg && i < 4; i++)
        if (base == 0 || info->seg[i].addr < base)
            base = info->seg[i].addr;
    return base;
}

bool anchor_matches(const ModuleInfo *info, uintptr_t base, const export_anchor_t *anchor) {
    const uintptr_t want = base + anchor->va;
    const size_t len = std::strlen(anchor->text) + 1;
    for (uint32_t i = 0; i < info->nseg && i < 4; i++) {
        const uintptr_t lo = info->seg[i].addr;
        const uintptr_t hi = lo + info->seg[i].size;
        if (want >= lo && want + len <= hi && (info->seg[i].prot & 1))
            return std::memcmp(reinterpret_cast<const void *>(want), anchor->text, len) == 0;
    }
    return false;
}

// The table of the firmware whose anchor sits in this library, or nullptr.
const export_table_t *module_table(int handle, const char *file, const export_table_t *tables,
                                size_t tables_n, uintptr_t *base) {
    ModuleInfo info;
    *base = module_base(handle, file, &info);
    if (*base == 0) {
        logf("hmd2: %s: no module info (handle 0x%x)", file, handle);
        return nullptr;
    }
    for (size_t k = 0; k < tables_n; k++) {
        if (anchor_matches(&info, *base, tables[k].anchor)) {
            logf("hmd2: %s: firmware table %d.%02d, base %p", file, tables[k].fw / 100,
                 tables[k].fw % 100, reinterpret_cast<void *>(*base));
            return &tables[k];
        }
    }
    logf("hmd2: %s: no export table for this firmware (base %p)", file,
         reinterpret_cast<void *>(*base));
    return nullptr;
}

// sceKernelDlsym first; at the export offsets of the firmware's table when the
// library does not export the name. `required` logs each missing export; an
// optional call returns false quietly, so its caller logs one line instead of
// one per missing export.
bool resolve_module(int handle, const char *file, const Slot *slots, size_t n,
                    const export_table_t *tables, size_t tables_n, const char **failed_name,
                    bool required = true) {
    uintptr_t base = 0;
    const export_table_t *table = nullptr;
    bool tried_table = false;
    for (size_t i = 0; i < n; i++) {
        *slots[i].slot = nullptr;
        if (sceKernelDlsym(handle, slots[i].name, slots[i].slot) == 0 && *slots[i].slot != nullptr)
            continue;
        if (!tried_table) {
            table = module_table(handle, file, tables, tables_n, &base);
            tried_table = true;
            if (table != nullptr && s.fw == 0)
                s.fw = table->fw;
        }
        for (size_t j = 0; table != nullptr && j < table->count; j++)
            if (std::strcmp(table->exports[j].name, slots[i].name) == 0)
                *slots[i].slot = reinterpret_cast<void *>(base + table->exports[j].offset);
        if (*slots[i].slot == nullptr) {
            if (failed_name != nullptr)
                *failed_name = slots[i].name;
            if (!required)
                return false;
            logf("hmd2: %s: %s not resolved", file, slots[i].name);
            return false;
        }
    }
    return true;
}

// libSceVrHand is loaded for the state report only (hand tracking is not part
// of C7 yet); failing to bind it is not fatal.
void bind_vrhand() {
    const int handle = load_sysmodule(0, "libSceVrHand");
    if (handle < 0) {
        logf("hmd2: libSceVrHand unavailable (handle 0x%x)", handle);
        return;
    }
    const char *missing = nullptr;
    if (resolve_module(handle, "libSceVrHand.sprx", kVrHandSlots, kSlots(kVrHandSlots),
                       kTables_libSceVrHand,
                       sizeof kTables_libSceVrHand / sizeof *kTables_libSceVrHand, &missing))
        logf("hmd2: libSceVrHand bound (%u exports)", (unsigned)kSlots(kVrHandSlots));
    else
        logf("hmd2: libSceVrHand not usable here, hand tracking stays off");
}

// -- textures ---------------------------------------------------------------
// GFX10 texture-descriptor codes, as PS5VR's eng_agc_writer.c names them.
constexpr uint32_t kFormat8_8_8_8Unorm = 56;
constexpr uint32_t kSelX = 4, kSelY = 5, kSelZ = 6, kSelW = 7;
constexpr uint32_t kRsrcImg2D = 9;
constexpr uint32_t kSw64kbRX = 27;

// A 2D texture descriptor pointing at a tiled B8G8R8A8 surface: format
// 8_8_8_8_UNORM with the Z,Y,X,W swizzle (memory B,G,R,A -> shader R,G,B,A)
// and the colour block's SW_64KB_R_X tiling, which is what the eye buffers
// below are laid out in. The field placement is eng_agc_writer.c's
// build_tsharp_2d_internal, unchanged except for the swizzle and the tiling.
bool build_eye_descriptor(uint32_t out[8], uint64_t address, uint32_t width, uint32_t height) {
    if (address == 0 || (address & 0xffffu) != 0 || width == 0 || height == 0)
        return false;
    const uint32_t wm1 = width - 1;
    std::memset(out, 0, 8 * sizeof(uint32_t));
    out[0] = static_cast<uint32_t>(address >> 8);
    out[1] = static_cast<uint32_t>(address >> 40) | (kFormat8_8_8_8Unorm << 20) | ((wm1 & 3u) << 30);
    out[2] = ((wm1 >> 2) & 0xfffu) | ((height - 1u) << 14);
    out[3] = kSelZ | (kSelY << 3) | (kSelX << 6) | (kSelW << 9) | (kRsrcImg2D << 28) |
             (kSw64kbRX << 20);
    out[5] = 7u << 20;
    return true;
}

// A 2D descriptor for a linear RGBA8 surface with pitch == width: the same
// fields HLA-PSVR2's tsharp_rgba8 fills (psvr2.c:1230-1237), which is their
// hardware-validated encoding. No tiling bits and the X,Y,Z,W swizzle (memory
// R,G,B,A -> shader R,G,B,A); a RADV host-visible buffer from vkMapMemory is
// exactly this, and the mapped address is what the console samples.
bool build_linear_descriptor(uint32_t out[8], uint64_t address, uint32_t width, uint32_t height) {
    if (address == 0 || (address & 0xffu) != 0 || width == 0 || height == 0 || width > 16384 ||
        height > 16384)
        return false;
    const uint32_t wm1 = width - 1;
    std::memset(out, 0, 8 * sizeof(uint32_t));
    out[0] = static_cast<uint32_t>(address >> 8);
    out[1] = static_cast<uint32_t>(address >> 40) | (kFormat8_8_8_8Unorm << 20) | ((wm1 & 3u) << 30);
    out[2] = (((wm1 >> 2) & 0xfffu) | ((height - 1u) << 14));
    out[3] = kSelX | (kSelY << 3) | (kSelZ << 6) | (kSelW << 9) | (kRsrcImg2D << 28);
    return true;
}

// The per-frame layer Hmd2 reads in SetParam: the layer type, the texture size,
// the sampler and the per-eye viewports (left half, right half of one
// side-by-side image), both eyes' field of view, the device pose and the
// predicted display time. Field placement as PS5VR's app_vr.c submit() writes
// it; that file is the only description of this structure.
void build_frame_layer(uint8_t out[kParamLayerBytes], const Pose &pose, uint64_t time,
                       uint64_t frame, const float fov[4]) {
    std::memset(out, 0, kParamLayerBytes);
    uint32_t *u = reinterpret_cast<uint32_t *>(out);
    const uint32_t tw = 2 * s.eye_w, th = s.eye_h;
    // The headset's own field of view, not an inflated one: HLA-PSVR2 passes
    // the render FOV with plain viewports (no overscan) and the headset fills
    // the edges itself. The right eye has out/in swapped.
    const float right[4] = {fov[1], fov[0], fov[2], fov[3]};

    u[0x00 / 4] = 0x10;                              // layer type: SBS eye pair
    u[0x08 / 4] = tw;
    u[0x0c / 4] = th;
    u[0x10 / 4] = tw;
    u[0x14 / 4] = th;
    u[0x18 / 4] = 0x92;                              // sampler: clamp modes
    u[0x1c / 4] = 0x00fff000;                        // sampler: max LOD
    u[0x20 / 4] = 0x05500000;                        // sampler: bilinear
    u[0x28 / 4] = s.eye_w;                           // left half: w, h
    u[0x2c / 4] = s.eye_h;
    u[0x38 / 4] = s.eye_w;                           // right half: w, h, x offset
    u[0x3c / 4] = s.eye_h;
    u[0x40 / 4] = s.eye_w;
    std::memcpy(out + 0x48, fov, 16);                // left eye tangents
    std::memcpy(out + 0x58, right, 16);              // right eye tangents
    std::memcpy(out + 0x68, pose.pos, 12);
    std::memcpy(out + 0x74, pose.quat, 16);
    std::memcpy(out + 0x88, &time, 8);               // predicted display time
    std::memcpy(out + 0x90, &frame, 8);
    u[0x98 / 4] = 100;
    u[0xa0 / 4] = s.eye_w;                           // render (output) size per eye
    u[0xa4 / 4] = s.eye_h;
    u[0xa8 / 4] = s.eye_w;
    u[0xac / 4] = s.eye_h;
}

// -- pose -------------------------------------------------------------------
// One eye pose at result+pos_off/quat_off. Valid only when the twelve bytes are
// finite metres inside the tracking volume; a tracker that reports zeroes or
// garbage leaves it untracked and the caller falls back to the head pose plus
// its own IPD.
bool read_eye_pose(const uint8_t *result, size_t pos_off, size_t quat_off, Pose *out) {
    float pos[3];
    float quat[4];
    std::memcpy(pos, result + pos_off, sizeof pos);
    std::memcpy(quat, result + quat_off, sizeof quat);
    for (int i = 0; i < 3; i++)
        if (!(pos[i] == pos[i]) || pos[i] > 20.0f || pos[i] < -20.0f)
            return false;
    for (int i = 0; i < 4; i++)
        if (!(quat[i] == quat[i]))
            return false;
    const float length = quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] +
                         quat[3] * quat[3];
    if (!(length > 0.25f) || length > 4.0f)
        return false;
    std::memcpy(out->pos, pos, sizeof pos);
    std::memcpy(out->quat, quat, sizeof quat);
    out->tracked = true;
    return true;
}

Pose query_pose(uint64_t time) {
    // An untracked result is reported as such, but with the last good values
    // rather than the origin: those are also the pose submitted with the frame
    // Hmd2 reprojects, and the caller holds its render pose. HLA-PSVR2 does the
    // same in sess_head() (xr_session.c:64-80): the last device and eye poses
    // stand in for a dropped tracker, so the image is warped from where it was
    // drawn, not from the tracking origin.
    Pose pose = s.held;
    pose.tracked = false;
    s.eye[0] = s.held_eye[0];
    s.eye[1] = s.held_eye[1];
    if (sceVrTracker2GetResult == nullptr || s.hmd < 0)
        return pose;
    uint8_t result[0x400];
    uint8_t param[0x20];
    std::memset(param, 0, sizeof param);
    *reinterpret_cast<uint32_t *>(param + 0x00) = 0x20;
    *reinterpret_cast<int32_t *>(param + 0x04) = s.hmd;
    *reinterpret_cast<uint64_t *>(param + 0x10) = time;
    std::memset(result, 0, sizeof result);
    if (sceVrTracker2GetResult(param, result) != 0)
        return pose;
    // +4 valid, +0x60 position, +0x70 orientation (x,y,z,w).
    if (*reinterpret_cast<uint32_t *>(result + 4) == 0)
        return pose;
    std::memcpy(pose.pos, result + 0x60, 12);
    std::memcpy(pose.quat, result + 0x70, 16);
    pose.tracked = true;

    // The eye poses are free, the tracker fills them in the same block: left at
    // +0xa0/+0xb0, right at +0xe0/+0xf0 (HLA-PSVR2 psvr2.c:681-718). Only kept
    // when both are valid and the pair is a head's width apart.
    Pose left = {{0, 0, 0}, {0, 0, 0, 1}, false};
    Pose right = left;
    if (read_eye_pose(result, 0xa0, 0xb0, &left) && read_eye_pose(result, 0xe0, 0xf0, &right)) {
        const float dx = right.pos[0] - left.pos[0];
        const float dy = right.pos[1] - left.pos[1];
        const float dz = right.pos[2] - left.pos[2];
        const float separation = dx * dx + dy * dy + dz * dz;
        if (separation >= 0.02f * 0.02f && separation <= 0.12f * 0.12f) {
            s.eye[0] = left;
            s.eye[1] = right;
            s.held_eye[0] = left;
            s.held_eye[1] = right;
        }
    }
    s.held = pose;
    return pose;
}

int pick_user() {
    // The user service answers nothing until it is initialised; without this
    // the lookups fail, the fallback 0xff reaches sceHmd2Open and it returns
    // 0x809b0001 (HLA-PSVR2 psvr2.c:1110-1112 initialises first). A second
    // initialise just returns "already initialised", which is fine.
    sceUserServiceInitialize(nullptr);
    int users[4] = {-1, -1, -1, -1};
    if (sceUserServiceGetLoginUserIdList(users) == 0 && users[0] >= 0)
        return users[0];
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0)
        return user;
    return 0xff;  // what PS5VR passes when it has no user
}

// -- teardown ---------------------------------------------------------------
// Undoes setup() in reverse order. Every step is guarded by the flag that says
// it succeeded, so it is safe on a half-initialised session (an init that
// failed part-way) and safe to call twice (the init error path and shutdown()).
void teardown() {
    s.active = false;
    s.frame_open = false;

    // The TV mirror goes first and on its own: while it is on, Hmd2 owns the
    // two work areas, so they are only given back after a disable that
    // succeeded (a failed one leaves them to the process's end, like every
    // other leak-on-timeout here). Before VR mode off, so the TV returns to the
    // console's own output while reprojection still runs.
    if (s.mirror_on) {
        const int rc = sceHmd2ReprojectionDisableMirroring();
        logf("hmd2: TV mirror off (0x%08x)", (unsigned)rc);
        if (rc == 0) s.mirror_on = false;
    }

    // Leave VR mode first, and independently of the VideoOut port: setup()
    // enables VR mode before it opens the port, so a failed open still has VR
    // mode to disable. Reprojection keeps sampling the last buffer, so it has
    // to stop before the buffers are unregistered.
    const bool had_port = s.vh >= 0;
    const bool had_vr_mode = s.vr_mode;
    int disable_rc = 0;
    if (had_vr_mode) {
        disable_rc = sceHmd2ReprojectionDisableVrMode();
        s.vr_mode = false;
    }

    int unregister_rc = 0;
    int close_rc = 0;
    if (had_port) {
        if (s.registered) {
            for (int i = 0; i < 50; i++) {
                unregister_rc = sceVideoOutUnregisterBuffers(s.vh, 0);
                if (unregister_rc != kVideoOutBusy)
                    break;
                sceKernelUsleep(10000);
            }
            s.registered = false;
        }
        close_rc = sceVideoOutClose(s.vh);
        s.vh = -1;
    }
    if (had_port) {
        logf("hmd2: stopped after %d frames (unregister 0x%08x close 0x%08x disable VR mode "
             "0x%08x)", s.submits, (unsigned)unregister_rc, (unsigned)close_rc,
             (unsigned)disable_rc);
    } else if (had_vr_mode) {
        logf("hmd2: VR mode off without a VideoOut port (disable 0x%08x)", (unsigned)disable_rc);
    }

    if (s.tracker_up || s.tracker_inited) {
        // Unregister the Sense controllers first: their pad handles are the
        // GetResult handles register_controller() took, and the tracker is
        // finalized below.
        for (int slot = 0; slot < 2; ++slot) {
            if (s.controllers[slot] >= 0 && sceVrTracker2UnregisterDevice != nullptr) {
                const int rc = sceVrTracker2UnregisterDevice(s.controllers[slot]);
                logf("hmd2: UnregisterDevice(%s Sense pad 0x%x) -> 0x%08x",
                     slot == 0 ? "right" : "left", s.controllers[slot], (unsigned)rc);
            }
            s.controllers[slot] = -1;
        }
        const int r1 = s.tracker_up ? sceVrTracker2UnregisterDevice(s.hmd) : 0;
        const int r2 = s.tracker_inited ? sceVrTracker2Finalize() : 0;
        logf("hmd2: tracker down (unregister 0x%08x finalize 0x%08x)", (unsigned)r1,
             (unsigned)r2);
        s.tracker_up = false;
        s.tracker_inited = false;
    }
    if (s.reproj_up) {
        const int r = sceHmd2ReprojectionTerminate();
        logf("hmd2: reprojection down (0x%08x)", (unsigned)r);
        s.reproj_up = false;
    }
    if (s.hmd >= 0) {
        const int r = sceHmd2Close(s.hmd);
        logf("hmd2: sceHmd2Close(0x%x) -> 0x%08x", s.hmd, (unsigned)r);
        s.hmd = -1;
    }
    if (s.hmd2_up) {
        const int r = sceHmd2Terminate();
        logf("hmd2: sceHmd2Terminate -> 0x%08x", (unsigned)r);
        s.hmd2_up = false;
    }
    s.inited = false;
    s.active = false;
    direct_free(&s.reproj_display, s.reproj_display_bytes, s.reproj_display_off);
    direct_free(&s.reproj_work, s.reproj_work_bytes, s.reproj_work_off);
    direct_free(&s.eye_mem, s.eye_mem_bytes, s.eye_mem_off);
    direct_free(&s.tracker_mem, s.tracker_mem_bytes, s.tracker_mem_off);
    s.reproj_display_bytes = s.reproj_work_bytes = s.eye_mem_bytes = s.tracker_mem_bytes = 0;
    s.reproj_display_off = s.reproj_work_off = s.eye_mem_off = s.tracker_mem_off = -1;
    // The mirror's work areas are Hmd2's while mirroring is on; a disable that
    // failed above means they are left to the process's end.
    if (!s.mirror_on) {
        direct_free(&s.mirror_display, s.mirror_display_bytes, s.mirror_display_off);
        direct_free(&s.mirror_work, s.mirror_work_bytes, s.mirror_work_off);
        s.mirror_display_bytes = s.mirror_work_bytes = 0;
        s.mirror_display_off = s.mirror_work_off = -1;
        s.mirror_display_query = 0;
    } else {
        logf("hmd2: TV mirror would not turn off; its work and display memory stay allocated");
    }
    s.mirror = false;
    s.image_bytes = 0;
    s.submits = 0;
    s.npending = 0;
    s.have_last = false;
    s.held = Pose{{0, 0, 0}, {0, 0, 0, 1}, false};
    s.held_eye[0] = s.held_eye[1] = s.held;
    s.eye[0] = s.eye[1] = s.held;
    s.last_begin_us = s.last_display_us = s.last_work_us = s.frame_started_us = 0;
    s.repeated = s.queue_full = s.missed = 0;
    s.pace_nap = s.pace_call = s.pace_ahead = s.pace_step = s.pace_n = 0;
    s.external = false;
    s.image_width = s.image_height = 0;
    for (int i = 0; i < kMaxBuffers; i++) {
        s.ext_address[i] = nullptr;
        s.ext_bytes[i] = 0;
    }
}

// -- setup ------------------------------------------------------------------
// After EnableVrMode the headset's USB-C DisplayPort link retrains for about a
// second; flips sent before the display scans out wedged Hmd2's queue for the
// rest of the process (HLA-PSVR2 saw it in a hosted title that flipped 1 ms
// after EnableVrMode). Wait until the VR port counts three vblanks, at most
// 5 s, and carry on either way: a display that never counts is still worth
// reporting and the rest of the run is bounded.
void wait_display_live() {
    uint64_t status[32] = {};
    const uint64_t started = sceKernelGetProcessTime();
    uint64_t first = ~0ull, count = 0;
    int rc = -1;
    while (sceKernelGetProcessTime() - started < 5000000) {
        std::memset(status, 0, sizeof status);
        rc = sceVideoOutGetVblankStatus(s.vh, status);
        count = status[0];
        if (rc == 0 && first == ~0ull)
            first = count;
        if (rc == 0 && count >= first + 3)
            break;
        sceKernelUsleep(5000);
    }
    logf("hmd2: display live after %.0f ms (vblank rc 0x%08x count %llu -> %llu)",
         (double)(sceKernelGetProcessTime() - started) / 1000.0, (unsigned)rc,
         (unsigned long long)(first == ~0ull ? 0 : first), (unsigned long long)count);
}

bool open_video_out() {
    const uint32_t tw = 2 * s.eye_w;
    for (int i = 0; i < s.buffers; i++) {
        uint32_t descriptor[8];
        if (s.external) {
            if (!build_linear_descriptor(descriptor,
                                         reinterpret_cast<uint64_t>(s.ext_address[i]), tw, s.eye_h))
                return fail("texture descriptor (linear RGBA8)", -1);
        } else {
            uint8_t *image =
                static_cast<uint8_t *>(s.eye_mem) + static_cast<size_t>(i) * s.image_bytes;
            if (!build_eye_descriptor(descriptor, reinterpret_cast<uint64_t>(image), tw, s.eye_h))
                return fail("texture descriptor", -1);
        }
        std::memset(s.layers[i], 0, kLayerBytes);
        *reinterpret_cast<uint32_t *>(s.layers[i]) = 0x10;   // the same texture twice
        std::memcpy(s.layers[i] + 0x04, descriptor, sizeof descriptor);
        std::memcpy(s.layers[i] + 0x24, descriptor, sizeof descriptor);
        s.sets[i].layers = s.layers[i];
        s.sets[i].count = 1;
    }
    Hmd2VideoOutBuffer buffers[kMaxBuffers];
    std::memset(buffers, 0, sizeof buffers);
    for (int i = 0; i < s.buffers; i++)
        buffers[i].data = &s.sets[i];

    uint8_t attribute[kAttrBytes];
    std::memset(attribute, 0, sizeof attribute);
    sceVideoOutSetBufferAttribute2(attribute, kVideoOutFormatB8G8R8A8, 0 /* 64 KiB R_X tiles */, tw,
                                   s.eye_h, 0, 0, 0);
    int rc = sceVideoOutRegisterBuffers2(s.vh, 0, 0, buffers, s.buffers, attribute, 0x10, nullptr);
    logf("hmd2: RegisterBuffers2 (%d x %ux%u) -> 0x%08x", s.buffers, tw, s.eye_h, (unsigned)rc);
    if (rc != 0)
        return fail("sceVideoOutRegisterBuffers2", rc);
    s.registered = true;

    const int c1 = sceVideoOutConfigureOutput(s.vh, kVrMode120Hz, nullptr, nullptr, 0);
    const int c2 = sceHmd2ReprojectionSetRenderConfig(1, 0, 0);   // half-rate rendering
    logf("hmd2: ConfigureOutput (120 Hz) -> 0x%08x, SetRenderConfig(half rate) -> 0x%08x",
         (unsigned)c1, (unsigned)c2);
    // The link has to be live before the first flip, not merely VR mode on.
    wait_display_live();
    return true;
}

// The TV mirror, Hmd2's social screen: the compositor puts the headset's view
// on the TV, so it also reaches the system's own recording. It is the sequence
// both reference runtimes use -- HLA-PSVR2's mirror_apply (psvr2.c:1239) and
// PS5VR's app_vr.c:529-566 -- with the same option word ({1}: the view, as
// Horizon Call of the Mountain sets it) and the two work areas Hmd2 asks to be
// handed. Applied before VR mode, as both do.
//
// Not fatal: a mirror that Hmd2 refuses costs the TV, not the session. Every
// pointer is checked (the exports are optional, and setup() only resolves them
// when MIRROR=1), every return code is checked, and each failure leaves the
// mirror off, logs one line and lets VR run.
void mirror_start() {
    if (s.mirror_on)
        return;
    if (sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign == nullptr ||
        sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign == nullptr ||
        sceHmd2ReprojectionSetMirroringOption == nullptr ||
        sceHmd2ReprojectionEnableMirroring == nullptr ||
        sceHmd2ReprojectionDisableMirroring == nullptr) {
        s.mirror = false;
        logf("hmd2: TV mirror off: the mirroring exports are not resolved; VR continues");
        return;
    }
    const SizeAlign work = sceHmd2ReprojectionGetMirroringWorkMemorySizeAlign();
    const SizeAlign display = sceHmd2ReprojectionGetMirroringDisplayBufferSizeAlign();
    logf("hmd2: mirror memory work 0x%llx (align 0x%llx), display 0x%llx (align 0x%llx)",
         (unsigned long long)work.size, (unsigned long long)work.align,
         (unsigned long long)display.size, (unsigned long long)display.align);
    if (work.size == 0 || display.size == 0) {
        s.mirror = false;
        logf("hmd2: TV mirror off: Hmd2 reported a zero work or display size; VR continues");
        return;
    }
    s.mirror_display_query = display.size;
    // PS5VR's alignment guard: the queried alignment is a hint, and direct
    // memory wants at least 64 KiB.
    const size_t work_align = work.align > 0x10000 ? work.align : 0x10000;
    const size_t display_align = display.align > 0x10000 ? display.align : 0x10000;
    if (s.mirror_work == nullptr)
        s.mirror_work = direct_alloc(work.size, work_align, "mirror work", &s.mirror_work_off,
                                     &s.mirror_work_bytes);
    if (s.mirror_display == nullptr)
        s.mirror_display = direct_alloc(display.size, display_align, "mirror display",
                                        &s.mirror_display_off, &s.mirror_display_bytes);
    if (s.mirror_work == nullptr || s.mirror_display == nullptr) {
        s.mirror = false;
        logf("hmd2: TV mirror off: its work or display memory is unavailable; VR continues");
        return;
    }
    const uint32_t option[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    const int option_rc = sceHmd2ReprojectionSetMirroringOption(option);
    void *param[2] = {s.mirror_work, s.mirror_display};
    const int enable_rc = sceHmd2ReprojectionEnableMirroring(param);
    s.mirror_on = enable_rc == 0;   // EnableMirroring owns the work areas, so it decides
    s.mirror = s.mirror_on;
    if (!s.mirror_on) {
        logf("hmd2: TV mirror off: option 0x%08x enable 0x%08x; VR continues",
             (unsigned)option_rc, (unsigned)enable_rc);
        return;
    }
    logf("hmd2: TV mirror on: option 0x%08x enable 0x%08x", (unsigned)option_rc,
         (unsigned)enable_rc);
}

bool setup(const Config &config) {
    if (s.inited)
        return true;
    s.failed = false;
    s.error = {"", 0};
    s.fw = 0;
    s.submits = 0;
    s.errors = 0;
    s.external = config.external != nullptr && config.external_count > 0;
    if (s.external) {
        if (config.external_count > kMaxBuffers) {
            s.error = {"external eye buffers", -1};
            logf("hmd2: %d external eye buffers, at most %d", config.external_count, kMaxBuffers);
            return false;
        }
        s.image_width = config.image_width;
        s.image_height = config.image_height;
        s.eye_w = s.image_width / 2;
        s.eye_h = s.image_height;
        s.buffers = config.external_count;
        s.image_bytes = static_cast<size_t>(s.image_width) * s.image_height * 4;
        bool ok = s.image_width >= 2 && s.image_height > 0 && (s.image_width & 1u) == 0;
        for (int i = 0; ok && i < s.buffers; i++) {
            s.ext_address[i] = config.external[i].address;
            s.ext_bytes[i] = config.external[i].bytes;
            if (s.ext_address[i] == nullptr || s.ext_bytes[i] < s.image_bytes ||
                (reinterpret_cast<uintptr_t>(s.ext_address[i]) & 0xffu) != 0)
                ok = false;
        }
        if (!ok) {
            s.error = {"external eye buffers (address or size)", -1};
            logf("hmd2: external eye buffers need %ux%u RGBA8, 256-byte aligned, %zu bytes each",
                 2 * s.eye_w, s.eye_h, s.image_bytes);
            return false;
        }
    } else {
        uint32_t eye = config.eye_size;
        if (eye < 128)
            eye = 128;
        eye = (eye + 127u) & ~127u;              // whole tiles
        if (eye > 4096)
            eye = 4096;
        s.eye_w = s.eye_h = eye;
        s.buffers =
            config.buffers < 1 ? 1 : config.buffers > kMaxBuffers ? kMaxBuffers : config.buffers;
    }
    s.user = config.user_id >= 0 ? config.user_id : pick_user();
    s.mirror = config.mirror;
    logf("hmd2: user %d, eye %ux%u x%d buffers%s", s.user, 2 * s.eye_w, s.eye_h, s.buffers,
         s.external ? " (external, linear RGBA8)" : "");

    // From here on every failure unwinds the steps that already succeeded, so
    // init() leaves no direct memory or driver state behind and can be retried
    // in the same process.
    auto abort_setup = []() -> bool {
        teardown();
        s.failed = true;
        return false;
    };

    const char *missing = nullptr;
    int handle = load_sysmodule(kSysmoduleHmd2, "libSceHmd2");
    if (handle < 0 || !resolve_module(handle, "libSceHmd2.sprx", kHmd2Slots, kSlots(kHmd2Slots),
                                      kTables_libSceHmd2,
                                      sizeof kTables_libSceHmd2 / sizeof *kTables_libSceHmd2,
                                      &missing)) {
        s.error.step = missing != nullptr ? missing : "load libSceHmd2";
        s.error.rc = handle < 0 ? handle : 0;    // the resolve log has the detail
        return abort_setup();
    }
    // The mirroring exports are optional: MIRROR=0 does not resolve them at
    // all, and a library or firmware without them leaves the mirror off rather
    // than aborting the session. One line says why; VR continues.
    if (s.mirror) {
        const char *mirror_missing = nullptr;
        if (!resolve_module(handle, "libSceHmd2.sprx", kHmd2MirrorSlots,
                            kSlots(kHmd2MirrorSlots), kTables_libSceHmd2,
                            sizeof kTables_libSceHmd2 / sizeof *kTables_libSceHmd2, &mirror_missing,
                            false /* optional */)) {
            s.mirror = false;
            logf("hmd2: TV mirror off: libSceHmd2 does not provide %s; VR continues",
                 mirror_missing != nullptr ? mirror_missing : "the mirroring exports");
        }
    }
    handle = load_sysmodule(kSysmoduleVrTracker2, "libSceVrTracker2");
    if (handle < 0 || !resolve_module(handle, "libSceVrTracker2.sprx", kVrTracker2Slots,
                                      kSlots(kVrTracker2Slots), kTables_libSceVrTracker2,
                                      sizeof kTables_libSceVrTracker2 / sizeof *kTables_libSceVrTracker2,
                                      &missing)) {
        s.error.step = missing != nullptr ? missing : "load libSceVrTracker2";
        s.error.rc = handle < 0 ? handle : 0;
        return abort_setup();
    }

    uint8_t init_param[16];
    std::memset(init_param, 0, sizeof init_param);
    int rc = sceHmd2Initialize(init_param);
    logf("hmd2: sceHmd2Initialize -> 0x%08x", (unsigned)rc);
    if (rc != 0) {
        fail("sceHmd2Initialize", rc);
        return abort_setup();
    }
    s.hmd2_up = true;

    float fov[16];
    std::memset(fov, 0, sizeof fov);
    rc = sceHmd2GetFieldOfViewWithoutHandle(fov);
    std::memcpy(s.fov, fov, sizeof s.fov);
    logf("hmd2: GetFieldOfViewWithoutHandle -> 0x%08x: out %.4f in %.4f top %.4f bottom %.4f",
         (unsigned)rc, (double)fov[0], (double)fov[1], (double)fov[2], (double)fov[3]);
    if (rc != 0 || s.fov[0] <= 0.0f || s.fov[2] <= 0.0f) {
        fail("sceHmd2GetFieldOfViewWithoutHandle", rc);
        return abort_setup();
    }

    s.hmd = sceHmd2Open(s.user, 0, 0, nullptr);
    uint8_t info[64];
    std::memset(info, 0, sizeof info);
    rc = sceHmd2GetDeviceInformation(info);
    logf("hmd2: sceHmd2Open -> handle 0x%x, GetDeviceInformation -> 0x%08x (status %u worn %u "
         "lens %u mm)", s.hmd, (unsigned)rc, *reinterpret_cast<uint32_t *>(info), info[20], info[21]);
    if (s.hmd < 0) {
        fail("sceHmd2Open", s.hmd);
        return abort_setup();
    }
    s.device.status = *reinterpret_cast<uint32_t *>(info);
    s.device.worn = info[20] != 0;
    s.device.lens_mm = info[21];

    // Tracker, profile 0 (the only profile open to apps).
    uint32_t query[8] = {0x20, 0};
    uint32_t answer[8] = {0};
    rc = sceVrTracker2QueryMemory(query, answer);
    if (rc != 0) {
        fail("sceVrTracker2QueryMemory", rc);
        return abort_setup();
    }
    logf("hmd2: tracker work memory 0x%x bytes, align 0x%x", answer[1], answer[2]);
    s.tracker_mem = direct_alloc(answer[1], answer[2] > 0x10000 ? answer[2] : 0x10000, "tracker",
                                 &s.tracker_mem_off, &s.tracker_mem_bytes);
    if (s.tracker_mem == nullptr) {
        fail("tracker direct memory", -1);
        return abort_setup();
    }
    uint8_t ip[0x48];
    std::memset(ip, 0, sizeof ip);
    *reinterpret_cast<uint32_t *>(ip + 0x00) = 0x48;
    *reinterpret_cast<int32_t *>(ip + 0x08) = 700;
    *reinterpret_cast<int32_t *>(ip + 0x0c) = 700;
    *reinterpret_cast<uint64_t *>(ip + 0x10) = 0x1f00;
    *reinterpret_cast<uint64_t *>(ip + 0x18) = 0x1f00;
    *reinterpret_cast<void **>(ip + 0x20) = s.tracker_mem;
    *reinterpret_cast<uint32_t *>(ip + 0x28) = answer[1];
    *reinterpret_cast<uint32_t *>(ip + 0x2c) = answer[2];
    rc = sceVrTracker2Initialize(ip);
    if (rc == 0)
        s.tracker_inited = true;
    const int rc2 = rc == 0 ? sceVrTracker2RegisterDevice(0, s.hmd) : -1;
    logf("hmd2: sceVrTracker2Initialize -> 0x%08x, RegisterDevice(0, headset) -> 0x%08x",
         (unsigned)rc, (unsigned)rc2);
    if (rc != 0) {
        fail("sceVrTracker2Initialize", rc);
        return abort_setup();
    }
    if (rc2 != 0) {
        fail("sceVrTracker2RegisterDevice", rc2);
        return abort_setup();   // the tracker is initialized: Finalize it
    }
    s.tracker_up = true;

    // Reprojection: work and display memory, then Initialize (once per process).
    const SizeAlign work = sceHmd2ReprojectionQueryBufferSizeAlign();
    const SizeAlign display = sceHmd2ReprojectionQueryDisplayBufferSizeAlign();
    logf("hmd2: reprojection memory work 0x%llx (align 0x%llx), display 0x%llx (align 0x%llx)",
         (unsigned long long)work.size, (unsigned long long)work.align,
         (unsigned long long)display.size, (unsigned long long)display.align);
    s.reproj_work = direct_alloc(work.size, 0x10000, "reprojection work", &s.reproj_work_off,
                                 &s.reproj_work_bytes);
    s.reproj_display = direct_alloc(display.size, 0x10000, "reprojection display",
                                    &s.reproj_display_off, &s.reproj_display_bytes);
    if (s.reproj_work == nullptr || s.reproj_display == nullptr) {
        fail("reprojection direct memory", -1);
        return abort_setup();
    }
    uint8_t rp[72];
    std::memset(rp, 0, sizeof rp);
    *reinterpret_cast<void **>(rp + 0x00) = s.reproj_work;
    *reinterpret_cast<void **>(rp + 0x08) = s.reproj_display;
    *reinterpret_cast<uint32_t *>(rp + 0x10) = 256;
    *reinterpret_cast<int32_t *>(rp + 0x14) = 0x1f00;
    *reinterpret_cast<uint32_t *>(rp + 0x18) = 1;
    rc = sceHmd2ReprojectionInitialize(rp, nullptr);
    logf("hmd2: sceHmd2ReprojectionInitialize -> 0x%08x", (unsigned)rc);
    if (rc != 0) {
        fail("sceHmd2ReprojectionInitialize", rc);
        return abort_setup();
    }
    s.reproj_up = true;

    // Eye buffers: one direct-memory block, one side-by-side image per buffer.
    // In external mode the caller's mapped RADV buffers take their place.
    if (s.external) {
        for (int i = 0; i < s.buffers; i++)
            logf("hmd2: eye buffer %d: %ux%u linear RGBA8 at %p (%zu bytes)", i, 2 * s.eye_w,
                 s.eye_h, s.ext_address[i], s.ext_bytes[i]);
    } else {
        const size_t per_row_tiles = (2 * static_cast<size_t>(s.eye_w) + 127u) / 128u;
        const size_t rows = (s.eye_h + 127u) / 128u;
        s.image_bytes = per_row_tiles * rows * 0x10000u;
        s.eye_mem = direct_alloc(s.image_bytes * static_cast<size_t>(s.buffers), 0x10000,
                                 "eye buffers", &s.eye_mem_off, &s.eye_mem_bytes);
        if (s.eye_mem == nullptr) {
            fail("eye buffer direct memory", -1);
            return abort_setup();
        }
        logf("hmd2: eye image %zu bytes (%ux%u, 64 KiB R_X), %d buffers", s.image_bytes, 2 * s.eye_w,
             s.eye_h, s.buffers);
    }

    bind_vrhand();

    // The TV mirror, before VR mode (HLA-PSVR2 psvr2_output_start does the same,
    // "as Horizon does"). Harmless to skip or to fail: VR does not depend on it.
    if (s.mirror) mirror_start();

    // The VideoOut side, after VR mode is enabled - as Horizon and PS5VR do.
    rc = sceHmd2ReprojectionEnableVrMode(kVrMode120Hz);
    s.vr_mode = rc == 0;
    s.vh = sceVideoOutOpen(0xff, 32, 0, nullptr);
    logf("hmd2: ReprojectionEnableVrMode -> 0x%08x, VideoOutOpen(0xff, bus 32) -> 0x%x",
         (unsigned)rc, s.vh);
    if (s.vh < 0) {
        fail("sceVideoOutOpen(bus 32)", s.vh);
        return abort_setup();   // VR mode is on: teardown disables it
    }
    if (!open_video_out())
        return abort_setup();   // registered buffers and the port are undone

    s.buf = 0;
    s.frame = 0;
    s.last_begin_us = s.last_display_us = s.last_work_us = s.frame_started_us = 0;
    s.npending = 0;
    s.have_last = false;
    s.live_logged = false;
    s.repeated = s.queue_full = s.missed = 0;
    s.pace_nap = s.pace_call = s.pace_ahead = s.pace_step = s.pace_n = 0;
    s.submits = 0;
    s.errors = 0;
    s.frame_open = false;
    s.error = {"", 0};
    s.failed = false;
    s.inited = true;
    s.active = true;
    logf("hmd2: ready (firmware table %d, %u exports)", s.fw,
         (unsigned)(kSlots(kHmd2Slots) + (s.mirror ? kSlots(kHmd2MirrorSlots) : 0)));
    return true;
}

// -- frame submission -------------------------------------------------------
uint64_t now_us() {
    return sceKernelGetProcessTime();
}

void drop_pending(uint64_t frame) {
    for (int i = 0; i < s.npending; i++)
        if (s.pending[i].frame == frame) {
            s.pending[i] = s.pending[--s.npending];
            return;
        }
}

// SetParam + SubmitFlip for one frame. In external mode the image was written
// by the GPU and the caller waited on its fence, so there is no cache
// writeback (HLA-PSVR2 flushes nothing after vkCmdCopyImageToBuffer + fence).
bool submit_flip(uint32_t buf, uint64_t frame, uint64_t display, const Pose &pose) {
    alignas(16) uint8_t layer[kParamLayerBytes];
    build_frame_layer(layer, pose, display, frame, s.fov);
    struct {
        void *layers;
        int32_t count;
        int32_t pad;
        void *ext;
    } param = {layer, 1, 0, nullptr};
    if (!s.external && s.eye_mem != nullptr)
        flush_cpu_writes(static_cast<uint8_t *>(s.eye_mem) + static_cast<size_t>(buf) * s.image_bytes,
                         s.image_bytes);
    const int r1 = sceHmd2ReprojectionSetParam(buf, frame, &param, nullptr);
    const int r2 = sceVideoOutSubmitFlip(s.vh, static_cast<int>(buf), 2, static_cast<int64_t>(frame));
    if (r1 != 0 || r2 != 0) {
        if (s.errors++ < 20)
            logf("hmd2: frame %llu SetParam 0x%08x SubmitFlip 0x%08x", (unsigned long long)frame,
                 (unsigned)r1, (unsigned)r2);
        s.error = {r1 != 0 ? "sceHmd2ReprojectionSetParam" : "sceVideoOutSubmitFlip",
                   r1 != 0 ? r1 : r2};
    }
    return r1 == 0 && r2 == 0;
}

// A frame Hmd2 opened that will get no image of its own: show the last image
// again with the pose it was drawn for. Left in the queue, such a frame never
// frees its slot and every later BeginFrame waits out its retries (HLA-PSVR2:
// 66 ms and 12 fps in HL:A).
void show_last(uint64_t frame, uint64_t display) {
    drop_pending(frame);
    s.repeated++;
    if (s.have_last) {
        submit_flip(s.last_buf, frame, display, s.last_pose);
        return;
    }
    const Pose still = {{0, 0, 0}, {0, 0, 0, 1}, false};
    submit_flip(0, frame, display, still);
}

}  // namespace

// -- public API -------------------------------------------------------------
void set_logger(Logger logger) {
    s.log = logger;
}

bool init(const Config &config) {
    return setup(config);
}

bool active() {
    return s.active;
}

bool wait_slot() {
    if (!s.active)
        return false;
    // The cadence starts from the previous BeginFrame, not a free-running
    // timer: a slow frame is not held back and a fast one is not queued
    // (HLA-PSVR2 psvr2.c:1431-1447, whose 18 ms-work case fell to 14.6 fps
    // when the wait was anchored anywhere else).
    const uint64_t entered = now_us();
    uint64_t nap = 0;
    if (s.last_begin_us != 0) {
        const uint64_t start = s.last_begin_us + kFrameUs - 300;
        if (start > entered) {
            nap = start - entered;
            if (nap > 3 * kFrameUs)
                nap = 3 * kFrameUs;
            sceKernelUsleep(static_cast<unsigned>(nap));
        }
    }
    s.pace_nap += nap;
    return true;
}

bool begin_frame() {
    if (!s.active)
        return false;
    if (s.frame_open)   // the last frame was never submitted: draw it again
        return true;
    const uint64_t frame = s.frame + 1;
    // Frames two or more behind this one that were begun and never submitted
    // are abandoned; give them the last image so Hmd2's queue keeps moving.
    for (int i = 0; i < s.npending;) {
        if (s.pending[i].frame + 2 <= frame)
            show_last(s.pending[i].frame, s.pending[i].display);
        else
            i++;
    }
    const uint64_t entered = now_us();
    int rc = sceHmd2ReprojectionBeginFrame(frame);
    for (int tries = 0; rc == kVrQueueFull && tries < kMaxPendingQueueFull; tries++) {
        s.queue_full++;
        sceKernelUsleep(1000);
        rc = sceHmd2ReprojectionBeginFrame(frame);
    }
    if (rc != 0) {
        if (s.errors++ < 20)
            logf("hmd2: BeginFrame(%llu) -> 0x%08x", (unsigned long long)frame, (unsigned)rc);
        s.error = {"sceHmd2ReprojectionBeginFrame", rc};
        return false;
    }
    s.frame = frame;
    uint64_t display = 0;
    sceHmd2ReprojectionGetPredictedDisplayTime(frame, &display);
    if (display == 0)
        display = now_us() + 20000;
    s.display_time = display;
    s.pose = query_pose(display);
    if (!s.live_logged && display > now_us() + 2000) {
        s.live_logged = true;
        logf("hmd2: Hmd2 live at frame %llu (display %+lld us)",
             (unsigned long long)frame, (long long)(display - now_us()));
    }
    const uint64_t done = now_us();
    s.pace_call += done - entered;
    s.pace_ahead += display > done ? display - done : 0;
    s.pace_step += (s.last_display_us != 0 && display > s.last_display_us)
                       ? display - s.last_display_us
                       : 0;
    if (s.last_display_us != 0 && display > s.last_display_us + kFrameUs * 3 / 2)
        s.missed += static_cast<int>((display - s.last_display_us) / kFrameUs) - 1;
    // Pace against Hmd2's display time when this app has slack: wake a period
    // plus the flip latency before it, so the frame is rendered just in time
    // and not queued. A frame that needs most of the period is not held at
    // all (HLA-PSVR2 xr_session.c:601-621).
    const bool slack = s.last_work_us == 0 || s.last_work_us * 5 / 4 < kFrameUs * 8 / 10;
    if (slack && display > done + kLeadUs) {
        uint64_t nap = display - kLeadUs - done;
        if (nap > 3 * kFrameUs)
            nap = 3 * kFrameUs;
        sceKernelUsleep(static_cast<unsigned>(nap));
        s.pace_nap += nap;
    }
    s.pace_n++;
    s.last_display_us = display;
    s.last_begin_us = entered;
    s.frame_started_us = now_us();
    if (s.npending == kPendingFrames)
        show_last(s.pending[0].frame, s.pending[0].display);
    s.pending[s.npending].frame = frame;
    s.pending[s.npending].display = display;
    s.npending++;
    s.frame_open = true;
    return true;
}

EyeBuffer acquire() {
    EyeBuffer buffer = {s.buf, nullptr, 2 * s.eye_w, s.eye_h};
    if (s.external && s.ext_address[s.buf] != nullptr) {
        buffer.pixels = static_cast<uint8_t *>(s.ext_address[s.buf]);
    } else if (s.eye_mem != nullptr) {
        buffer.pixels = static_cast<uint8_t *>(s.eye_mem) + static_cast<size_t>(s.buf) * s.image_bytes;
    }
    return buffer;
}

bool submit() {
    if (!s.frame_open)
        return false;
    s.frame_open = false;
    const bool ok = submit_flip(static_cast<uint32_t>(s.buf), s.frame, s.display_time, s.pose);
    drop_pending(s.frame);
    s.have_last = true;
    s.last_buf = static_cast<uint32_t>(s.buf);
    s.last_pose = s.pose;
    s.last_work_us = now_us() - s.frame_started_us;
    s.submits++;
    s.buf = (s.buf + 1) % s.buffers;
    return ok;
}

Pose predicted_pose() {
    return s.pose;
}

Poses predicted_poses() {
    Poses poses = {s.pose, s.eye[0], s.eye[1]};
    return poses;
}

Pose predicted_pose(uint64_t display_time_us) {
    if (!s.active)
        return Pose{{0, 0, 0}, {0, 0, 0, 1}, false};
    return query_pose(display_time_us);
}

bool register_controller(int tracker_handle, bool right_hand) {
    if (!s.tracker_inited || sceVrTracker2RegisterDevice == nullptr || tracker_handle < 0)
        return false;
    const int slot = right_hand ? 0 : 1;
    if (s.controllers[slot] == tracker_handle) return true;
    if (s.controllers[slot] >= 0) {
        if (sceVrTracker2UnregisterDevice != nullptr)
            sceVrTracker2UnregisterDevice(s.controllers[slot]);
        s.controllers[slot] = -1;
    }
    // The device ids are HLA-PSVR2's: 1 for the right Sense controller and 2 for
    // the left, registered against the scePad handle that GetResult then takes
    // (psvr2.c:469). UNVERIFIED on hardware (see hmd2.h).
    const int type = right_hand ? 1 : 2;
    const int rc = sceVrTracker2RegisterDevice(type, tracker_handle);
    logf("hmd2: RegisterDevice(%d, %s Sense pad 0x%x) -> 0x%08x", type,
         right_hand ? "right" : "left", tracker_handle, (unsigned)rc);
    if (rc != 0) return false;
    s.controllers[slot] = tracker_handle;
    return true;
}

bool controller_pose(int tracker_handle, float pos[3], float quat[4]) {
    if (pos != nullptr) pos[0] = pos[1] = pos[2] = 0.0f;
    if (quat != nullptr) {
        quat[0] = quat[1] = quat[2] = 0.0f;
        quat[3] = 1.0f;
    }
    if (pos == nullptr || quat == nullptr || !s.tracker_inited ||
        sceVrTracker2GetResult == nullptr || tracker_handle < 0) {
        return false;
    }

    // The query block is the head pose's (query_pose above, HLA-PSVR2's
    // tracker_get at psvr2.c:672-682): size 0x20, handle at +4, time at +0x10.
    // A Sense controller's handle is its scePad handle, after
    // register_controller() registers that same handle.
    const uint64_t time = s.display_time != 0 ? s.display_time : sceKernelGetProcessTime();
    uint8_t param[0x20];
    std::memset(param, 0, sizeof param);
    *reinterpret_cast<uint32_t *>(param + 0x00) = 0x20;
    *reinterpret_cast<int32_t *>(param + 0x04) = tracker_handle;
    *reinterpret_cast<uint64_t *>(param + 0x10) = time;
    uint8_t result[0x400];
    std::memset(result, 0, sizeof result);
    if (sceVrTracker2GetResult(param, result) != 0) return false;
    // +4 valid, +0x60 position, +0x70 orientation (x, y, z, w): the same
    // offsets HLA-PSVR2 reads for the controller pose (psvr2.c:843-851).
    if (*reinterpret_cast<uint32_t *>(result + 4) == 0) return false;
    Pose decode;
    if (!read_eye_pose(result, 0x60, 0x70, &decode)) return false;
    const float length = std::sqrt(decode.quat[0] * decode.quat[0] + decode.quat[1] * decode.quat[1] +
                                   decode.quat[2] * decode.quat[2] + decode.quat[3] * decode.quat[3]);
    for (int i = 0; i < 4; ++i) quat[i] = decode.quat[i] / length;
    for (int i = 0; i < 3; ++i) pos[i] = decode.pos[i];
    return true;
}

uint64_t display_time_us() {
    return s.display_time;
}

Fov field_of_view() {
    Fov fov = {s.fov[0], s.fov[1], s.fov[2], s.fov[3]};
    return fov;
}

DeviceState device_state() {
    if (s.hmd < 0)
        return s.device;
    const uint64_t now = sceKernelGetProcessTime();
    if (now - s.worn_checked >= 500000) {
        s.worn_checked = now;
        uint8_t info[64];
        std::memset(info, 0, sizeof info);
        if (sceHmd2GetDeviceInformation(info) == 0) {
            s.device.status = *reinterpret_cast<uint32_t *>(info);
            const bool worn = info[20] != 0;
            if (worn != s.device.worn)
                logf("hmd2: headset %s", worn ? "put on" : "taken off");
            s.device.worn = worn;
            s.device.lens_mm = info[21];
        }
    }
    return s.device;
}

bool worn() {
    return device_state().worn;
}

int eye_size() {
    return static_cast<int>(s.eye_w);
}

int eye_height() {
    return static_cast<int>(s.eye_h);
}

int buffer_count() {
    return s.buffers;
}

size_t image_bytes() {
    return s.image_bytes;
}

bool mirroring() {
    return s.mirror_on;
}

size_t mirror_display_bytes() {
    return s.mirror_display_query;
}

int refresh_hz() {
    return 120;
}

int frames_submitted() {
    return s.submits;
}

int frame_errors() {
    return s.errors;
}

int repeated_frames() {
    return s.repeated;
}

int queue_full_waits() {
    return s.queue_full;
}

int missed_frames() {
    return s.missed;
}

PaceStats take_pace() {
    const float n = s.pace_n != 0 ? static_cast<float>(s.pace_n) : 1.0f;
    PaceStats stats = {static_cast<float>(s.pace_nap) / n / 1000.0f,
                       static_cast<float>(s.pace_call) / n / 1000.0f,
                       static_cast<float>(s.pace_ahead) / n / 1000.0f,
                       static_cast<float>(s.pace_step) / n / 1000.0f};
    s.pace_nap = s.pace_call = s.pace_ahead = s.pace_step = s.pace_n = 0;
    return stats;
}

FlipInfo flip_status() {
    FlipInfo info = {0, -1, -1};
    if (s.vh < 0)
        return info;
    uint64_t status[16] = {};
    info.rc = sceVideoOutGetFlipStatus(s.vh, status);
    if (info.rc == 0)
        info.shown = status[3];   // the argument of the latest flip shown
    info.pending = sceVideoOutIsFlipPending(s.vh);
    return info;
}

int firmware_table() {
    return s.fw;
}

int user_id() {
    return s.user;
}

Error error() {
    return s.error;
}

void shutdown() {
    // Undo every step setup() got to, in reverse order: VR mode even when the
    // VideoOut port never opened, registered buffers, the port, tracker,
    // reprojection, Hmd2, then the direct memory. teardown() is also the init
    // error path, so a failed init leaves nothing behind and can be retried.
    teardown();
}

// -- eye buffer drawing -----------------------------------------------------
// 64 KiB tiles of 128x128 pixels, tile index row-major, then the in-tile
// address of the colour block's SW_64KB_R_X swizzle (the formula PS5_Vulkan's
// CPU renderer uses to fill VideoOut buffers, src/demo_renderer.cpp).
size_t pixel_offset(uint32_t x, uint32_t y, uint32_t width) {
    const uint32_t offset = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^
                            ((y << 8) & 0x4000u) ^ ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^
                            ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
    const uint32_t tiles_per_row = (width + 127u) >> 7;
    const uint32_t tile = (y >> 7) * tiles_per_row + (x >> 7);
    return (static_cast<size_t>(tile) << 16) + offset;
}

uint32_t pixel(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(r) << 16) | 0xff000000u;
}

uint32_t get_pixel(const EyeBuffer &buffer, uint32_t x, uint32_t y) {
    if (buffer.pixels == nullptr || x >= buffer.width || y >= buffer.height)
        return 0;
    uint32_t value;
    std::memcpy(&value, buffer.pixels + pixel_offset(x, y, buffer.width), sizeof value);
    return value;
}

void put_pixel(const EyeBuffer &buffer, uint32_t x, uint32_t y, uint32_t value) {
    if (buffer.pixels == nullptr || x >= buffer.width || y >= buffer.height)
        return;
    std::memcpy(buffer.pixels + pixel_offset(x, y, buffer.width), &value, sizeof value);
}

void fill_rect(const EyeBuffer &buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint32_t value) {
    if (x >= buffer.width || y >= buffer.height)
        return;
    const uint32_t right = w > buffer.width - x ? buffer.width : x + w;
    const uint32_t bottom = h > buffer.height - y ? buffer.height : y + h;
    for (uint32_t row = y; row < bottom; row++)
        for (uint32_t column = x; column < right; column++)
            put_pixel(buffer, column, row, value);
}

}  // namespace hmd2
