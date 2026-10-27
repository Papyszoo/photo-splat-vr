# Spike B step 2: PSVR2 bindings (C7) and a headset test pattern

Task B2, 2026-10-26. The console was **off** for this task: everything below was
built and checked on the Mac against the payload SDK (v0.45) and the PS5VR
sources in `$SPLAT_DATA_DIR/ref/PS5VR`. Nothing here has run on hardware yet.
What to type tomorrow, what to expect and what each failure means is section 8;
the plan for Spike B step 3 (RADV images into these buffers) is section 9.

## 1. What was built

| Path | What it is |
| --- | --- |
| `platform/ps5/hmd2/hmd2.h` | the API: `init()`, `predicted_pose()`, `acquire()`/`submit()`, `shutdown()` and the buffer-drawing helpers |
| `platform/ps5/hmd2/hmd2.cpp` | the bindings: module loading, Hmd2/VrTracker2/VideoOut, the frame path (see section 2 for what came from where) |
| `platform/ps5/hmd2/hmd2_exports.h` | generated: per-firmware export offsets + anchor strings for 13.60 and 7.61/7.20 |
| `platform/ps5/hmd2/NOTICE` | pointer to `THIRD_PARTY_NOTICES.md`, where the PS5VR and PS5_Vulkan/ps5-native-app-boilerplate notices now live |
| `platform/ps5/hmd2/Makefile` | builds `libhmd2.a` |
| `platform/ps5/vr_hello/main.cpp` | the payload: test pattern + 1 Hz status for 30 s |
| `platform/ps5/vr_hello/Makefile` | builds `vr_hello.elf`, links `libhmd2.a` |
| `tools/export-tables/tables.json` | the offsets and anchors, per library and firmware |
| `tools/export-tables/gen_export_table.py` | `tables.json` → C header, and decrypted `.sprx` → table entry / `--verify` |
| `tools/export-tables/README.md` | the scheme, the data table, how to add a firmware |

Build (both, in this order):

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/hmd2          # libhmd2.a
make -C platform/ps5/vr_hello      # vr_hello.elf (links the library)
```

`vr_hello.elf` is 166 KiB, an x86-64 FreeBSD PIE like `hello.elf`. Both outputs
are gitignored.

## 2. What was ported from where

PS5VR is GPL-3.0-or-later, Copyright (C) 2026 Husam Osman; this project is
GPL-3.0-or-later, so the code is reused and the notices are in the top-level
`THIRD_PARTY_NOTICES.md` under "Ported into the PS5 payloads" (task F2 merged
them there; `platform/ps5/hmd2/NOTICE` is now a pointer). The clone used is the
`mtp-24` fork of `theghostonline/PS5VR` (`$SPLAT_DATA_DIR/ref/PS5VR`), same
licence.

| From PS5VR | To `hmd2.cpp` | Taken over |
| --- | --- | --- |
| `app/src/app_vr.c` `load_sysmodule`, `module_base`, `anchor_matches`, `module_table`, `resolve_module` | same names | `sceSysmoduleLoadModule` + `sceKernelLoadStartModule("/<sandbox word>/common/lib/<lib>.sprx")`, dlsym first, then this firmware's export offsets checked by the anchor string in the read-only segment |
| `app_vr.c` `setup()` | `setup()` | `sceHmd2Initialize`, `GetFieldOfViewWithoutHandle`, `Open`, `GetDeviceInformation`, the VrTracker2 `QueryMemory`/`Initialize`/`RegisterDevice(0, headset)` parameter blocks (`0x48` bytes, `0x1f00` flags), the reprojection work/display memory + `ReprojectionInitialize` (`72`-byte param) |
| `app_vr.c` `app_vr_start()` | `setup()` + `open_video_out()` | `EnableVrMode(0x2000f)`, `sceVideoOutOpen(0xff, 32, 0, NULL)`, one layer set per buffer, `SetBufferAttribute2(0x8000…, tiling 0, 2*eye, eye)`, `RegisterBuffers2(vh, 0, 0, bufs, 3, attr, 0x10, NULL)`, `ConfigureOutput(0x2000f)`, `SetRenderConfig(1, 0, 0)` |
| `app_vr.c` `begin_frame()`, `submit()`, `wait_slot()` | `begin_frame()`, `submit()`, `wait_slot()` | `BeginFrame(frame)` with the `0x8A72001A` (queue full) retry, `GetPredictedDisplayTime`, the pose query through `sceVrTracker2GetResult` (`+4` valid, `+0x60` position, `+0x70` quaternion), the `0x148`-byte per-frame layer with its field placement, `SetParam(buffer, frame, …)`, `SubmitFlip(vh, buffer, 2, frame)`, and the fixed 60 Hz cadence (`16667 µs`) |
| `app_vr.c` `app_vr_worn()` | `device_state()`/`worn()` | `sceHmd2GetDeviceInformation` polled every 500 ms: `+0` status, `+20` worn, `+21` lens distance |
| `app_vr.c` `app_vr_stop()`, `app_vr_shutdown()` | `shutdown()` | the order matters and is kept: `DisableVrMode`, unregister buffers retrying `0x80290009` (busy) 50× 10 ms, `VideoOutClose`, tracker `UnregisterDevice`/`Finalize`, `ReprojectionTerminate`, `Hmd2Close`, `Hmd2Terminate` |
| `app/engine/media/src/eng_agc_writer.c` `build_tsharp_2d_internal`, `eng_agc_build_tsharp_render_target` | `build_eye_descriptor()` | the 8-dword GFX10 2D descriptor: format code, `(w-1)&3<<30`, `(w-1)>>2 | (h-1)<<14`, the swizzle/type word, `SW_64KB_R_X << 20`, `7<<20` |
| `app/src/vr_exports_1360.h`, `vr_exports_761.h` | `tools/export-tables/tables.json` | the offsets and anchors themselves (13.60 and 7.61/7.20) |

Adapted, not copied: PS5VR's eye buffers are AGC render targets; ours are
direct memory filled from the CPU. The texture descriptor is therefore built
for a **B8G8R8A8** surface (the BGRA swizzle, `eng_agc_build_tsharp_bgra8`'s
case) rather than an RGBA8 one.

Also ported, from a different project:

| From | To | What |
| --- | --- | --- |
| `PS5_Vulkan/src/demo_renderer.cpp` (`ps5-native-app-boilerplate`, GPL-3.0+, Copyright (C) 2026 BlackBearReloaded / Mihawk-99) | `hmd2::pixel_offset` | the 64 KiB-tiled (`SW_64KB_R_X`) address of a pixel, unchanged apart from taking the surface width as a parameter. That project fills VideoOut buffers from the CPU with it, which is exactly this problem |

The idea that makes the payload possible is ours: Hmd2 samples whatever
texture the layer descriptor describes, so a CPU-written tiled surface in
direct memory can stand in for an AGC render target. Section 7 lists what is
still assumed about that.

## 3. The API

```cpp
namespace hmd2 {
void set_logger(void (*logger)(const char *line));   // set before init
bool init(const Config &config);   // loads, opens, registers; false on failure
                                   // (the failure is cleaned up, so retry works)

struct Config { int user_id = -1; uint32_t eye_size = 1024; int buffers = 3; };
struct Pose   { float pos[3]; float quat[4]; bool tracked; };
struct Fov    { float out, in, top, bottom; };       // half-angle tangents
struct EyeBuffer { int index; uint8_t *pixels; uint32_t width, height; };
struct DeviceState { uint32_t status; bool worn; uint32_t lens_mm; };
struct Error  { const char *step; int32_t rc; };

bool wait_slot();          // hold the 60 Hz cadence
bool begin_frame();        // Hmd2 BeginFrame + predicted display time + pose
EyeBuffer acquire();       // this frame's buffer (side-by-side, tiled BGRA)
bool submit();             // SetParam + SubmitFlip, advance the buffer
Pose predicted_pose();     // pose for the frame begin_frame() opened
Pose predicted_pose(uint64_t display_time_us);  // pose at an explicit time
uint64_t display_time_us();
Fov field_of_view();       DeviceState device_state();   bool worn();
int eye_size();  int buffer_count();  size_t image_bytes();
int refresh_hz();  int frames_submitted();  int frame_errors();
int firmware_table();      Error error();
void shutdown();           // stops VideoOut, then Hmd2, then frees memory

size_t pixel_offset(uint32_t x, uint32_t y, uint32_t width);
uint32_t pixel(uint8_t r, uint8_t g, uint8_t b);        // B8G8R8A8 memory order
uint32_t get_pixel(const EyeBuffer &, uint32_t x, uint32_t y);
void put_pixel(const EyeBuffer &, uint32_t x, uint32_t y, uint32_t value);
void fill_rect(const EyeBuffer &, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t value);
}
```

`init()` logs every step with its return code, so a failure is one grep away:
`grep '(failed)' <log>` names the step, and `error()` has the same pair. A
failure unwinds every step that had already succeeded (VR mode even when the
VideoOut port never opened, tracker/reprojection/Hmd2 handles, direct memory),
so `init()` can be called again in the same process; `shutdown()` after success
uses the same unwind in reverse order.

The frame loop is `wait_slot → begin_frame → acquire → draw → submit`, and
`acquire()` hands back the buffer index Hmd2 will read (`SetParam`'s first
argument), so the caller never touches VideoOut.

## 4. The buffer path in detail

**VideoOut.** `sceVideoOutOpen(0xff, 32, 0, NULL)` opens Hmd2's "VR driver"
port. `0xff` is the user PS5VR passes; the VideoOut attribute is
`sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000, 0, 2*eye, eye, 0, 0, 0)`:
format `0x8000…` is B8G8R8A8 SDR (`PS5_PayloadSDK`'s `videoout.h`, measured by
PS5_Vulkan), tiling 0 is 64 KiB tiles of 128×128 four-byte pixels — the colour
block's `SW_64KB_R_X`. Three buffers, registered with
`sceVideoOutRegisterBuffers2(vh, 0, 0, bufs, 3, attr, 0x10, NULL)`, each
`data` pointing at a one-entry layer set (`{layers, 1}`). The same attribute and
the same buffer layout are what PS5_Vulkan's WSI registers for the TV
(`ps5vk_video_out_register`: "tiled B8G8R8A8 images"), so for the display half
this is a layout the console is known to scan out from CPU-written memory.

**The layer.** At registration each layer is 0x130 bytes: `+0x00` layer type
`0x10`, then the *same* side-by-side texture descriptor at `+0x04` and `+0x24`
(the layer carries one texture twice; the per-eye halves are cut out by the
per-frame viewports). The descriptor is 8 dwords: `addr>>8`, `addr>>40 |
format<<20 | (w-1)&3<<30`, `((w-1)>>2) | (h-1)<<14`, `swizzle | type | 
SW_64KB_R_X<<20`, `0` (pitch implied by tiling), `7<<20`, `0`, `0`. Format 56
is `8_8_8_8_UNORM`; the swizzle is Z,Y,X,W because memory is B,G,R,A.

Per frame, `SetParam` gets a *different* 0x148-byte layer that carries no
texture pointers — only the dynamic part: type `0x10`, texture size, the three
sampler dwords (`0x92`, `0x00fff000`, `0x05500000`), the two eye viewports,
both eyes' tangents (`fov * 1.15` overscan, right eye with in/out swapped), the
device pose, the predicted display time, the frame number, `100`, and the
per-eye render size. `SetParam(buffer, frame, …)` selects the registered buffer
by index, so the textures come from registration and everything else from the
call. That split is PS5VR's, not ours; the field map is only described by that
source (section 7, item 6).

Then `sceVideoOutSubmitFlip(vh, buffer, 2, frame)`.

**Tiling and why a CPU fill works.** The eye image is
`tiles_per_row × tiles_down × 64 KiB`, tile index row-major, and the byte
inside a tile from the `R_X` swizzle (formula in `pixel_offset`). The direct
memory is allocated with `sceKernelAllocateDirectMemory(…, WB_ONION)` and
`sceKernelMapDirectMemory(…, 0x33)`, and the mapping address is what the GPU
samples, which is why the descriptor can point straight at it.

WB_ONION is cacheable, and the GPU and the display controller read direct
memory straight from DRAM (PS5VR's `eng_agc_runtime_cache_flush` comment says
so), so CPU writes have to be written back before Hmd2 samples them. `submit()`
does exactly what both reference projects do: `clflush` on every 64-byte line of
the image, then `mfence` (PS5VR's `eng_agc_runtime_cache_flush`, PS5_Vulkan's
`ps5vk_flush_cpu_cache`). At 1024 per eye that is 131k `clflush` per frame,
comfortably inside the 16.7 ms budget — and it is not optional: skipping it
gives stale or torn content and looks exactly like a format bug.

**Rate.** 120 Hz output with `SetRenderConfig(1, …)` (half-rate rendering): each
submitted frame is shown for two refreshes, so our 60 Hz submissions land on a
fixed 16.667 ms cadence — PS5VR's setting, and the one Horizon uses.

## 5. Export tables

`libSceHmd2`, `libSceVrTracker2` and `libSceVrHand` do not export the functions
C7 needs, so they are called at fixed offsets inside one build of one firmware.
`tables.json` holds the offsets for **13.60** (also 13.20) and **7.61**
(also 7.20 — PS5VR generated 7.61's table and documents 7.20's libraries as
identical), plus the anchor string that must sit at a known address in the
library's read-only segment before any offset is used. `libSceVrHand` has only
the 13.60 table, as in PS5VR: hand tracking is not part of C7 (it is loaded and
bound for the state report only).

At runtime: dlsym is tried for every name first; if any name is missing, the
firmware's table is looked up by anchor, and `hmd2` logs
`firmware table 13.60, base 0x…` or `no export table for this firmware`. The
second message on a 13.60 console would mean the library build is not what
PS5VR's table was generated from.

Generation and the procedure for adding a firmware are in
`tools/export-tables/README.md`. The header is generated from the JSON, so a
new firmware is: decrypted libraries → `--sprx` → paste into `tables.json` →
`--emit-c` → rebuild.

## 6. Static verification (no console)

Done here:

- `libhmd2.a` and `vr_hello.elf` build with the payload SDK v0.45, `-Wall
  -Werror`, clang 21, on macOS arm64. The library needs no PS5 headers, so it
  also compiles on the host (`-undefined dynamic_lookup`) — which is how the
  tiling check below ran.
- Every imported symbol resolves against the SDK stubs at link time, and each
  one exists in the stub library it belongs to:
  `libkernel_web.so` (`sceKernelAllocateDirectMemory`, `MapDirectMemory`,
  `Munmap`, `ReleaseDirectMemory`, `GetModuleInfo`, `LoadStartModule`,
  `GetFsSandboxRandomWord`, `Dlsym`, `Usleep`, `GetProcessTime`,
  `sysctlbyname`), `libSceVideoOut.so` (`Open`, `Close`,
  `SetBufferAttribute2`, `RegisterBuffers2`, `UnregisterBuffers`,
  `SubmitFlip`, `ConfigureOutput`), `libSceSysmodule.so`
  (`sceSysmoduleLoadModule`), `libSceUserService.so` (`GetLoginUserIdList`,
  `GetInitialUser`). `nm -u` on the ELF lists exactly these.
- The tiled layout: `pixel_offset` is a bijection over 2048×1024, 1024×512 and
  4096×2048 surfaces (no address out of range, none used twice, none unused),
  and `fill_rect` over the whole surface touches exactly every 4-byte slot. A
  wrong in-tile swizzle would still be self-consistent — this proves the layout
  is a proper tiling, not that the swizzle matches the colour block. Section 8
  is how that gets settled.
- `gen_export_table.py --emit-c` reproduces the committed header, and its NID
  hash agrees with PS5VR's self-test (`sceHmd2Initialize` → `c812oYs7Vsc`). The
  `--sprx` path was smoke-tested on an SDK stub library (a real ELF64 with a
  dynamic symbol table, plain names): offsets and anchor found as expected. It
  has not seen a decrypted PS5 system library, which is what it exists for.
- The cache writeback is in the built ELF (`clflush` and `mfence` appear in
  `llvm-objdump -d`); whether it is sufficient is a headset question.

Not verifiable offline: whether a payload may load `/system/common/lib/*.sprx`
and open VideoOut bus 32 at all, whether Hmd2 accepts a 1024-per-eye
CPU-filled surface, and every return code in between. That is tomorrow's run.

## 7. Open questions

1. **Eye-buffer size Hmd2 accepts.** We start at 1024 per eye (PS5VR's floor,
   cheap to fill: 8 MiB per side-by-side image, three buffers). PS5VR defaults
   to 4096 and Horizon uses 4096; whether Hmd2 *requires* something near the
   panel resolution is unknown. If `RegisterBuffers2` or `SetParam` rejects
   1024, change `Config` in `vr_hello/main.cpp` to 2048/4096 and rebuild (one
   line); the library logs the size it used.
2. **90/120 Hz.** Only `0x2000f` (PS5VR's `VR_MODE_120HZ`) is used, for both
   `EnableVrMode` and `ConfigureOutput`. Whether a 90 Hz mode exists, and
   whether half-rate rendering has a 90 Hz equivalent, is unknown; nothing in
   PS5VR or PS5_Vulkan documents one.
3. **Buffer formats Hmd2 accepts.** PS5VR ships RGBA8, RGB10A2 (10-bit) and
   RGBA16F (HDR, experimental) AGC targets. We use 8-bit B8G8R8A8 with 64 KiB
   R_X tiling. Linear surfaces, 10-bit and FP16 from a payload are untested;
   10-bit would need the RGB10A2 descriptor and a different CPU fill.
4. **Does Hmd2 sample the layer's descriptor or the VideoOut attribute?** Both
   describe the same memory in the same layout here, so this run cannot tell
   them apart — by design. One hint that the descriptor governs: PS5VR passes
   the same `0x8000…` attribute for its 8-, 10- and 16-bit eye buffers, which a
   single attribute format could not describe. If the picture is wrong, the
   first thing to try is flipping the descriptor swizzle (`kSelZ | kSelY<<3 |
   kSelX<<6` for R/B), the second is a different tiling.
5. **Payload privileges.** PS5VR is an installed title; a `payload.elf` process
   has to be allowed to `sceSysmoduleLoadModule` those libraries, load system
   `.sprx` files and open bus 32. No evidence either way yet. The neighbouring
   `probe_agc` payload answers the same question for AGC imports, so compare
   its result: if neither payload starts, it is the loader and not Hmd2.
6. **The layer field map is inferred** from PS5VR's `submit()`, which is the
   only description of it: two dwords (`+0x24`, `+0x44`) are never written,
   `+0x98 = 100` is unidentified, and the eye viewport block's boundaries are
   guesses. If the picture arrives but is mispositioned or squashed, this is
   where to look — and a wrong `0x148` layer is more likely than a wrong
   texture.
7. **TV mirroring** is not implemented (PS5VR's `SetMirroringOption` +
   `EnableMirroring`). Expect the TV to show its own UI or nothing while the
   headset shows the pattern.
8. **Ordering, for when C7 moves into a title.** PS5VR calls its module preload
   *before* the app opens its sandbox, because the libraries load through
   `/<sandbox random word>/common/lib/`. A payload never changes root, so this
   is fine today; the title port in Phase 2 must call `hmd2::init()` early.
9. **Flip rate**: `sceVideoOutSetFlipRate` is not called (PS5_Vulkan calls it on
   the TV port; PS5VR does not for the VR port). If `SubmitFlip` blocks or
   times out, try flip rate 0.
10. **IPD/lens distance** are Hmd2's business; we report the lens distance and
    never set it.

## 8. Tomorrow on the console

Prerequisites: PSVR2 connected, powered and **on the head** when the payload
starts (Hmd2 `Open` needs a live headset), console on 13.60, ProsperoMgr
running (it is), nothing else using the headset. `run.sh` starts `klogsrv`
itself.

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/hmd2 && make -C platform/ps5/vr_hello

platform/ps5/scripts/run.sh platform/ps5/vr_hello/vr_hello.elf 40
```

40 s of capture for a 30 s run. `run.sh` exits 0 when the start line appears
(the pattern is the ELF stem, `vr_hello`, which the first line contains); the
log lands in `$SPLAT_DATA_DIR/runs/console/<stamp>-vr_hello.log`. To read only
the interesting lines: `grep -E 'vrhello|hmd2' <log>`.

### Expected log

Return codes and the structure are what success looks like; the pose, FOV and
lens numbers are examples and will differ.

```
<118>[payload.elf] vrhello: start (vr_hello.elf, ps5-splat-vr, spike B step 2)
<118>[payload.elf] vrhello: firmware 13.60 (kern.sdk_version 0x13600007)
<118>[payload.elf] hmd2: user 1, eye 2048x1024 x3 buffers
<118>[payload.elf] hmd2: libSceHmd2: sysmodule 0x12a -> 0x00000000
<118>[payload.elf] hmd2: libSceHmd2: load "/<word>/common/lib/libSceHmd2.sprx" -> handle 0x4 (res 0x00000000)
<118>[payload.elf] hmd2: libSceHmd2.sprx: firmware table 13.60, base 0x200000000
<118>[payload.elf] hmd2: libSceVrTracker2: ... handle 0x5 ...
<118>[payload.elf] hmd2: libSceVrTracker2.sprx: firmware table 13.60, base 0x...
<118>[payload.elf] hmd2: sceHmd2Initialize -> 0x00000000
<118>[payload.elf] hmd2: GetFieldOfViewWithoutHandle -> 0x00000000: out 0.9367 in 0.8... top 0.9... bottom 0.9...
<118>[payload.elf] hmd2: sceHmd2Open -> handle 0x2, GetDeviceInformation -> 0x00000000 (status 0 worn 1 lens 20 mm)
<118>[payload.elf] hmd2: tracker work memory 0x... bytes, align 0x...
<118>[payload.elf] hmd2: sceVrTracker2Initialize -> 0x00000000, RegisterDevice(0, headset) -> 0x00000000
<118>[payload.elf] hmd2: reprojection memory work 0x... (align 0x...), display 0x... (align 0x...)
<118>[payload.elf] hmd2: sceHmd2ReprojectionInitialize -> 0x00000000
<118>[payload.elf] hmd2: eye image 8388608 bytes (2048x1024, 64 KiB R_X), 3 buffers
<118>[payload.elf] hmd2: libSceVrHand: load "..." -> handle 0x6 (res 0x00000000)
<118>[payload.elf] hmd2: libSceVrHand bound (5 exports)
<118>[payload.elf] hmd2: ReprojectionEnableVrMode -> 0x00000000, VideoOutOpen(0xff, bus 32) -> 0x2
<118>[payload.elf] hmd2: RegisterBuffers2 (3 x 2048x1024) -> 0x00000000
<118>[payload.elf] hmd2: ConfigureOutput (120 Hz) -> 0x00000000, SetRenderConfig(half rate) -> 0x00000000
<118>[payload.elf] hmd2: ready (firmware table 1360, 16 exports)
<118>[payload.elf] vrhello: eye buffer 2048x1024 x3, B8G8R8A8 tiled 64 KiB R_X, 8388608 bytes each, fov out 0.9367 in 0.8032 top 0.9124 bottom 0.8923
<118>[payload.elf] vrhello: t=1s frames=61 60.9 fps state=0 worn=1 lens=20 mm eye=2048x1024 bufs=3 out=120Hz pose=(0.012,1.604,-0.021) yaw=+0.4 pitch=-0.7 tracked=1 err=0 fw=1360
...
<118>[payload.elf] hmd2: stopped after 1800 frames (unregister 0x00000000 close 0x00000000 disable VR mode 0x00000000)
<118>[payload.elf] hmd2: tracker down (unregister 0x00000000 finalize 0x00000000)
<118>[payload.elf] hmd2: reprojection down (0x00000000)
<118>[payload.elf] hmd2: sceHmd2Close(0x2) -> 0x00000000
<118>[payload.elf] hmd2: sceHmd2Terminate -> 0x00000000
<118>[payload.elf] vrhello: done, 1800 frames in 30 s, 30 s elapsed
```

### What you should see

Left eye: dark green background (green-dominant), right eye: dark blue
(green kept low, blue high), so a swapped eye or a single-eye picture is
unmistakable. Both eyes' colours come from one function, so all you compare is
which half is green. Both: a 64-pixel grid, a white crosshair in
the centre, four white corner squares, a bright band along the top edge, and a
yellow vertical bar with a 48-pixel square on it that **moves as you turn your
head** — left when you look left, right when you look right, half-way across the
eye at 45°. The picture is head-locked (it is a 2D test pattern, not a 3D
scene), so the bar moving is the pose pipeline working.

`frames=60`/s and `out=120Hz` in the log; `tracked=1`; `state=0`; `err=0`.

### What each failure means

| Where it stops | Log | Meaning / next step |
| --- | --- | --- |
| nothing at all | no `vrhello:` line in the captured log | the payload never started. Either the loader could not resolve its imports (`libSceVideoOut`/`libSceSysmodule`/`libSceUserService` — cross-check with `probe_agc`), or 9021/upload failed (`run.sh` exit 4). If probe_agc starts and this does not, the difference is the library set |
| module load | `hmd2: libSceHmd2: … -> handle 0x8…` (negative) | `sceKernelLoadStartModule` refused: a payload may not load system `.sprx`, or the sandbox path differs. Then C7 has to run as a title (PS5VR's context); nothing else in this library can work |
| module load | `hmd2: …: no module info (handle …)` | the module loaded but `sceKernelGetModuleInfo` would not describe it: looks like a privilege problem too |
| table | `no export table for this firmware (base 0x…)` | the library build is not the one PS5VR's table came from. Note the base and the firmware; a table for that build is needed (`tools/export-tables/README.md`) |
| table | `…: sceHmd2Xxx not resolved` | dlsym missed it and the table does not list it: a name gap, not a firmware mismatch |
| Hmd2 | `sceHmd2Initialize -> 0x8…` | Hmd2 refused to start: headset not connected/ready, or something else owns it. Reboot the headset, power-cycle it, retry |
| Hmd2 | `sceHmd2Open -> handle 0xffffffff` | no headset session for user N (logged just before): headset off/not worn, or the payload's user is wrong. Try `Config::user_id = 0` or the console's actual user |
| FOV | `GetFieldOfViewWithoutHandle -> 0x8…`, or a zero tangent | Hmd2 has no valid display: it did not see the headset's panel |
| tracker | `sceVrTracker2QueryMemory/Initialize -> 0x8…` | the tracker service is unavailable (cameras/headset USB); Hmd2 alone can display but there is no pose |
| tracker | `RegisterDevice(0, headset) -> 0x8…` | Hmd2 handles 0 is not the headset, or the tracker already has a device |
| reprojection | `reprojection direct memory (…) -> 0x8…` | direct memory exhausted/refused (WB_ONION). The log prints every block and then the total in use |
| reprojection | `sceHmd2ReprojectionInitialize -> 0x8…` | memory too small (compare the logged sizes), or another process already initialised reprojection |
| VR mode | `ReprojectionEnableVrMode -> 0x8…` | VR mode refused: headset not in use/no display, or another VR app is running |
| VideoOut | `VideoOutOpen(0xff, bus 32) -> 0x8…` | no VR port: Hmd2 is not in VR mode (see above), or bus 32 does not exist for a payload |
| buffer | `RegisterBuffers2 (3 x 2048x1024) -> 0x8…` | **the interop question**: Hmd2/VideoOut rejected the attribute, size or tiling. Try 2048 then 4096 per eye; then a different tiling; then the format |
| output mode | `ConfigureOutput (120 Hz) -> 0x8…` but registration ok | 120 Hz not accepted (the console needs the title to declare HFR). The run continues at the port's default rate; note the code |
| per frame | `hmd2: BeginFrame(… ) -> 0x8…` | the reprojection queue would not give a frame (queue full is retried 40× 1 ms). If it repeats, VR mode is not really up |
| per frame | `hmd2: frame N SetParam 0x… SubmitFlip 0x…` | the layer was rejected or the flip is busy — **the second interop question**. SetParam failing points at the `0x148` layer layout; SubmitFlip at the buffer/flip mode. First 20 occurrences are logged, all are counted in `err=` |
| picture | several lines, but the pattern looks wrong | in order: (a) **R and B swapped** → the descriptor swizzle is the other way; (b) **stale/torn/banded content** → the cache writeback did not happen or the tiling write is wrong; (c) **blocky diagonal mess** → wrong in-tile swizzle (`pixel_offset` vs the colour block); (d) **misplaced or squashed halves** → the per-eye viewport dwords in the `0x148` layer; (e) **only one eye lit** → the second texture descriptor at `+0x24`. Every one of these is a different bug, so note which it is |
| removal | `headset taken off, stopping after N frames` | expected when you take it off; the shutdown lines follow |
| shutdown | `unregister 0x80290009` in the stopped line | the buffers stayed busy through 50 × 10 ms; harmless at exit, but it means a flip was still pending |

## 9. Spike B step 3: getting RADV-rendered images into these buffers

The question the spec leaves open is how a `VkImage` becomes the texture Hmd2
samples. Four options, most promising first. The first one is not speculative:
PS5_Vulkan's WSI already does the allocation half of it on the TV port.

### Option 1 (recommended): render directly into VideoOut buffers

`PS5_Vulkan/driver/ps5vk_wsi.c` (`ps5vk_video_out_register`) allocates direct
memory itself with `ps5vk_direct_mapping_create(…, PS5VK_DIRECT_DISPLAY)`,
registers *that address* with `sceVideoOutRegisterBuffers2` (format `0x8000…`,
tiling 0), and the swapchain's `VkImage`s are the registered buffers: the file's
header says the images are "two 32 MiB tiled B8G8R8A8 images in one 64 MiB
direct-memory allocation", and `ps5vk_QueuePresentKHR` reads
`video->buffers.address + index * IMAGE_BYTES` as the presented image. So on
this hardware RADV already renders into CPU-mapped, VideoOut-registered, tiled
B8G8R8A8 memory and presents it by flip — the exact layout `hmd2` fills by hand
today. That is as close to proof as an offline check gets.

What is missing is only the **address**: the driver picks it and does not tell
the app. Changes:

1. **PS5_Vulkan (driver, small patch).** A device proc that returns the CPU/GPU
   address of a `VkDeviceMemory` (and its size): look up
   `ps5vk_device_memory` → `mapping.address` in the PS5 winsys. ~20 lines plus
   a `vkGetDeviceProcAddr` case, or a `VK_..._ps5_memory_address` extension
   struct. This is the only patch outside our own code and it is a read-only
   getter.
2. **`hmd2` (external buffer mode, ~1 day).** `Config` gains
   `struct Image { uint64_t address; uint32_t width, height; int format; }`
   and, when it is set, `init()` skips the direct-memory eye buffers and builds
   the descriptors/layer sets from those addresses instead. Everything after
   that — attribute, registration, SetParam, SubmitFlip, pose, worn, shutdown —
   is unchanged. The eye-buffer size question then applies to RADV's image
   instead of ours.
3. **`vr_hello` grows a RADV mode.** Create 2×1024×2048 (per eye ×2) images with
   `R8G8B8A8_UNORM`, `TILING_OPTIMAL`, `COLOR_ATTACHMENT`, bind them to the
   driver-allocated memory, draw the same test pattern with a trivial pipeline
   (the vkgs shader skeleton is already in the repo), then per frame:
   `begin_frame` → render into the acquired image → a queue wait so Hmd2 sees a
   finished image → `submit`. First version: `vkDeviceWaitIdle` per frame; the
   driver's own model — a wait packet at the start of the submission that
   renders into a buffer (`ps5vk_queue.c`, R69) and the flip submitted behind it
   — is what replaces it when a fence is imported.

Risks and their tells: the tiling must be `SW_64KB_R_X` (8-bit 4-channel colour,
no MSAA, no DCC — RDNA2's default for that format, but must be confirmed);
RADV's DCC metadata tail would be sampled as garbage (**try
`RADV_DEBUG=nodcc` first** — one line, and it separates the two failure
modes); and Hmd2 may sample before the GPU is done (the wait above).

### Option 2: GPU copy into an Hmd2-owned buffer

RADV renders into its own image; a compute or copy pass writes it into a buffer
whose address we already control (our direct memory). Needs the same address
export as option 1 for at least one side, plus a `vkCmdCopyImageToBuffer` /
`vkCmdCopyBufferToImage` pair (tiled→linear→tiled, our descriptor can be made
linear for the intermediate) and a semaphore between the copy and
`SubmitFlip`. Costs one extra pass and one copy per frame, and it avoids DCC
entirely. This is the option to take if option 1's images turn out to be
un-sampleable by Hmd2 (e.g. RADV insists on a metadata layout we cannot
describe).

### Option 3: CPU readback

`vkCmdCopyImageToBuffer` into a host-visible linear buffer, wait for the fence,
then tile with `pixel_offset` into the Hmd2 buffer with `memcpy`. No driver
change at all, and it reuses everything built today. Cost: a GPU→CPU sync,
8 MiB per frame memcpy at 1024 per eye (about 1 ms at 60 Hz on this CPU) and no
overlap — fine as a correctness bridge and as the fallback if options 1-2 both
fail.

### Option 4 (last): render with AGC, as PS5VR does

Port PS5VR's AGC renderer (`eng_agc_runtime.c`, its clean-room
`sce_agc.h`, LLPC-compiled shaders). It is known to work on this hardware and
would make the eye buffers AGC render targets again, but it replaces the C6
design (RADV, vkgs, the radix sort) with a completely different API for the
whole renderer. Only if 1-3 all fail.

Whichever wins, `hmd2` keeps its public API; only `Config` grows the external
buffers and `vr_hello` becomes the interop test instead of the payload test.

## 10. Follow-ups

Task F2 (2026-10-26) resolved the review items here:

- `platform/ps5/hmd2/NOTICE` is merged into `THIRD_PARTY_NOTICES.md`
  ("Ported into the PS5 payloads": PS5VR, and ps5-native-app-boilerplate via
  PS5_Vulkan); the per-directory file is now a pointer.
- `platform/ps5/README.md` has a payloads table with the `hmd2` library and
  `vr_hello` rows, plus the build commands. Add the "what is on the console"
  line after the first successful run.
- `hmd2::init()` is no longer one-shot: a failed init releases its direct
  memory and driver state, and can be retried (it also takes
  `predicted_pose(display_time_us)` now). No backlog entry needed for the
  payload; the title port still wants init before the sandbox is opened
  (section 7, item 8).

Remaining `docs/BACKLOG.md` candidate: the `0x148` layer field map is inferred
from one source and should be re-derived if a second source appears.
