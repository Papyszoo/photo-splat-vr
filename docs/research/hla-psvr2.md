# HLA-PSVR2: how a homebrew title drives PSVR2 (Spike B step 3)

Task R3, 2026-10-26. Source: shallow clone at `$SPLAT_DATA_DIR/ref/HLA-PSVR2`,
commit `160b8e2` ("Half-Life: Alyx PS VR2 1.0.1"), 57 files, read-only —
nothing of it was built or run. Paths below are relative to that clone.

It is a complete OpenXR runtime for PSVR2 ("PS5XR") plus the Wine bridge around
it: HL:A speaks OpenVR → `openvr_api.dll` (OpenComposite) → `wineopenxr.dll` (PE)
→ `wineopenxr.prx` (Unix, PS5XR linked in) → Hmd2. The part that matters is
`ps5xr/runtime/src/psvr2.c` (headset, tracking, pads, pacing) and `xr_vulkan.c`
(swapchains + compositor): the Vulkan → eye-buffer path, in production at 60→120 Hz.

## 1. Frame path to the headset

`xrWaitFrame` → `psvr2_begin_frame` (`xr_session.c:604`, `psvr2.c:1417`): nap,
`sceHmd2ReprojectionBeginFrame(frame)`, `…GetPredictedDisplayTime`, then nap until
`display - period - latency - 1500 µs` when the app has slack (`xr_session.c:601-621`).
The app renders into swapchain images (`XR_KHR_vulkan_enable`) and ends with one
`XrCompositionLayerProjection`; `xrEndFrame` → `vk_compose` (`xr_vulkan.c:685`) copies
both eyes into one side-by-side host-visible buffer, submits on the app's queue and
waits on a fence (`:789-820`). `psvr2_submit` → `submit_flip` (`psvr2.c:1514`, `:1558`)
calls `sceHmd2ReprojectionSetParam(buf, frame, layer, NULL)` and
`sceVideoOutSubmitFlip(vh, buf, 2, frame)` (`:1582-1583`); a frame ended without an image
is re-submitted with the last one (`:1549`), else its queue slot never frees and every
`BeginFrame` is queue-full (their log: 12 fps).

**Eye buffer**: one texture per buffer, both eyes side by side, **linear RGBA8** —
`tsharp_rgba8` (`psvr2.c:1230-1237`) sets no tiling bits and the identity X,Y,Z,W
swizzle, and `xr_vulkan.c:5-11` calls it "Hmd2's linear T#"; 8-bit, MSAA resolved before
the copy (`xr_vulkan.c:463`). Size from `eye_size()` (`xr_instance.c:598-608`): panel
2000x2040 × `eye_scale`, rounded to 64×32 texels so the SBS pitch stays 256-byte
aligned; `eye_scale = 1.0` (`vrtitle/ps5xr.ini:5`) → **1984x2048 per eye**,
3968×2048×4 B ≈ 31 MiB per buffer, 3 buffers — the README's "locked 60 fps at 1984x2048
per eye", headset filling in to 120 Hz.

**Registration**: `EnableVrMode(0x2000f)` → `sceVideoOutOpen(0xff, 32, 0, NULL)` (bus
32) → `SetBufferAttribute2(attr, 0x8000…, tiling 0, 2*eye_w, eye_h)` →
`RegisterBuffers2(vh, 0, 0, bufs, 3, attr, 0x10, NULL)`, each buffer a one-entry layer
set whose type-`0x10` layer carries the same descriptor twice (`psvr2.c:1318-1365`); the
per-frame `0x148` layer adds viewports, tangents, pose, display time and frame id
(`:1567-1581`).

**Refresh and reprojection**: `VR_MODE_90HZ = 0x2000c`, `VR_MODE_120HZ = 0x2000f`
(`psvr2.c:103-104`); 60-rendered-to-120 is `ConfigureOutput(0x2000f)` +
`SetRenderConfig(1,0,0)` (`apply_mode` `:1276-1289`), switchable at run time (`:1372`).
Lens distortion, late-latch reprojection and TV mirroring are Hmd2's; the app gives
pose, tangents and FOV only.

**Two ordering rules they learned expensively**: after `EnableVrMode` the DP link
retrains ~1 s and flips before the display scans out **wedge Hmd2's queue for the rest
of the process** — they wait for 3 vblanks (`wait_display_live`, `psvr2.c:1296-1316`);
and Hmd2 spaces display times by the app's own frame interval, so waiting for the
display inside `BeginFrame` feeds back (18 ms work → 50 ms waits → 14.6 fps,
`:1431-1435`) — pace in `xrWaitFrame`.

## 2. GPU API and how images reach the headset

**Vulkan via RADV**; no AGC rendering anywhere. Native: RADV linked in, reached
through its ICD entry (`xr_vulkan.c:18-36`); hosted: the app's `libvulkan` supplies
`vkGetInstanceProcAddr` (`wineopenxr/unix/bridge_unix.c:189-202`). AGC is only
initialised, never used to draw — its ordering matters (§6). Handoff: 3 `VkBuffer`s of
`2*eye_w*eye_h*4` B, `HOST_VISIBLE|HOST_COHERENT`, preferring `DEVICE_LOCAL`, then
`vkMapMemory` (`xr_vulkan.c:303-338`); RADV's winsys maps direct memory (WB_ONION) at
one VA, so **the mapped pointer is the address the T# points at**. Copy is `vkCmdCopyImageToBuffer`
when the eye image is RGBA8, 1 sample and eye-sized, else a `vkCmdBlitImage` through a
2-layer scratch (scale, sRGB, format — `:740-784`). Sync: one command buffer + fence per
output buffer, `QueueSubmit` then `WaitForFences(…, 500 ms)` **before**
`SetParam`/`SubmitFlip` (`:798-822`) — a full CPU↔GPU sync per frame, logged; no
semaphore export, no address extension, no cache flush (the GPU wrote it).

## 3. Head pose and controllers

`sceVrTracker2GetResult({size 0x20, handle, time})` → 0x400 B: valid +4, device pos
+0x60, quat +0x70, lin vel +0x80, ang vel +0x90, **left eye +0xa0/+0xb0, right eye
+0xe0/+0xf0** (`psvr2.c:681-718`); we read only the device pose today, the eye poses
are free. Prediction: requested time clamped to `now + 40 ms`, one sample cached per
`XrTime` (`xr_session.c:47-63`), display time always from Hmd2; their latency
constants (4160 µs at 120 Hz, 5555 at 90 Hz, `xr_session.c:601`) are a sanity range
for our pacing. Sense: `scePadOpen(user, 3=left/4=right)` + `sceVrTracker2RegisterDevice(2=left,
1=right, pad)` (`psvr2.c:458-473`), state via `scePadVrControllerReadState` (0x68 B:
buttons +0x00, stick +0x04/05, analog triggers +0x08/09, touches +0x0c, connected
+0x40, `:517-581`); headset = type 0. Throw velocity is derived from consecutive poses
(0.5 EMA), not the tracker fields after +0x90 (`:795-825`); adaptive triggers are off
by default (`scePadVrControllerSetTriggerEffects`, 13.60 offset 0xa730, `:948-1031`).
**VrTracker2 faults if the process ends with it live**, so every exit path calls
`ps5xr_host_shutdown()` (`ps5xr_host.h:6-9`, `bridge_unix.c:230-235`,
`vr_title_shutdown` in the patch).

## 4. Packaging, runtime, budget, firmware

**Title, not payload**: `/data/homebrew/PPSA99198` (`eboot.bin` fSELF, `sce_module/`,
`sce_sys/`, `pcvr/`), registered by ShadowMountPlus, started from the home tile;
`/data` and USB come from a per-title helper (Lapy) — the split we already chose.
**param.json VR identity**: `attribute: 0x62000000`, `attribute3: 0x4080c40` = 0x400
PSVR2 app | 0x800 VR-only | 0x80040 120 Hz | 0x4000000 hand tracking
(`ps5xr/tools/make_title.py:5-9`). Not cosmetic: with `attribute 0` and
`attribute3 0x80c40`, "Hmd2 reported the headset not worn and never consumed a frame
(queue full from the 8th)" (`vrtitle/stage.py:79-88`) — **ours has both 0**
(`platform/ps5/title/param.json:12-14`). VR sysmodules must be preloaded before the
`/data` grant (`sceSysmoduleLoadModule(0x12A/0x12B)` at the top of `main`); after it
the load fails `PRX_PROCESS_STARTUP_FAILURE` (`vrtitle/patches/0001-…patch:704-711`,
`psvr2.c:138-141`). **Budget**: title app pool 11-12 GiB direct, flexible 448 MiB;
the binding limit is **open files** — a title holds ~250, the game 458 archive chunks,
so they patch VPK handles and raise `RLIMIT_NOFILE` (`bridge_unix.c:91-129`); our
loader must stay well below that. **Firmware 13.60 only**, "tested" (README:37);
another firmware needs its own export table. A title never calls `exit()`: they end via
`sceSystemServiceLoadExec`.

## 5. Licence

`GPL-3.0-or-later` (README:138-141, `LICENSE`). This project is
GPL-3.0-or-later too, so **we may reuse
code** with source + notice, as we already do for PS5VR in `THIRD_PARTY_NOTICES.md`.
Vendored: RADV/Mesa MIT, Wine LGPL-2.1+, OpenComposite GPL-3.0, OpenXR headers
Apache-2.0, gbe_fork LGPL-3.0, Lapy MIT; `psvr2.c` itself is their GPL-3 code.

## 6. What this means for Spike B step 3

**Confirmed.** Hmd2 samples the T# the layer describes, and a mapped RADV buffer is a
valid eye texture — **the driver patch in `spike-b-hmd2.md` §9 option 1 is
unnecessary**: the `vkMapMemory` pointer is the descriptor address. **90 Hz exists**
(`0x2000c`) and 120/60 is `SetRenderConfig(1,…)`: spike-b-hmd2 open question 2
answered, their production path being our target mode. Our `0x148` map matches theirs
field for field (`psvr2.c:1567-1581`) — second source, so that backlog item can close.
Tiling is not required either: their surface is linear RGBA8, ours 64 KiB-tiled, so the
descriptor governs and `tiling 0` in the VideoOut attribute does not describe it (question 4).

**Wrong or missing in ours**: (1) `param.json` `attribute`/`attribute3` = 0, so a title
never gets a frame; (2) no vblank wait after `EnableVrMode` (`hmd2.cpp:780-790`), so one
early flip can wedge the queue for the whole run; (3) fixed 60 Hz `wait_slot` cadence
(`hmd2.cpp:821-833`) instead of Hmd2's predicted display time, and no re-submit of
begun-but-unsubmitted frames; (4) tangents × 1.15 (`hmd2.cpp:433-436`) where they pass the
real render FOV with plain viewports — ours may be zoomed or clamped; (5) eye poses
(result +0xa0…) unused, and `clflush` is needed only while the CPU writes the buffer.

**Ordered plan, one run per step, one log line each:**

1. `param.json`: `attribute 0x62000000`, `attribute3 0x4080c40` — pass: `worn 1 status 0`,
   no `BeginFrame -> 0x8a72001a` in 300 frames.
2. Vblank gate in `hmd2::setup` after `ConfigureOutput` (3 counts, ≤5 s) — pass:
   `display live after N ms`, `err=0`.
3. External-buffer mode in `hmd2`: `Config.image{address,w,h}` skips the direct allocation
   and builds a linear RGBA8 descriptor — pass: `eye buffer 3968x2048 linear RGBA8 at 0x…`.
4. `vr_hello` fills 3 host-visible `VkBuffer`s by CPU through `vkMapMemory` and submits, no
   driver change and no pipeline. **This is the interop gate** — pass: the pattern in the
   headset, `frames=60 err=0 out=120Hz`.
5. Move the fill to the GPU (`CmdClearColorImage`/`CmdCopyImageToBuffer`, one fence, then
   submit) — pass: `gpu wait 1.2 ms`, picture unchanged.
6. Pacing: nap to `display - period - 4160 - 1500`, re-submit abandoned frames — pass:
   60.0 fps, `queue-full 0`, an 18 ms spin does not collapse to 15 fps.
7. 90 Hz native: `EnableVrMode(0x2000c)`, `ConfigureOutput(0x2000c)`, no `SetRenderConfig`
   — pass: `out=90Hz frames=90 err=0`.

Steps 1-2 gate all title-side VR work; step 4 is the Spike B gate. If step 4 fails while
direct-memory fills work, diff our descriptor against `tsharp_rgba8` (`psvr2.c:1230-1237`).

Read from the clone's `README.md`, `ps5xr/runtime/src/*`, `make_title.py`,
`wineopenxr/unix/bridge_unix.c`, `vrtitle/*`, `build.sh`, `deps.sh` (line numbers above);
our side: `spike-b-hmd2.md`, `spike-b-memory.md`, `platform/ps5/{hmd2,vr_hello,title}`.
