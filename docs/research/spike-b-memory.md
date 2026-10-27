# Spike B, step 1b: how PS5LM/PS5VR get GPU memory, and the route for our app

Research for Task R2, 2026-10-26. **Read-only**: the console was off; nothing was
run on it, nothing was installed. Sources are the clones under
`$SPLAT_DATA_DIR/ref/` (PS5LM, PS5VR, PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK,
elfldr, ShadowMountPlus, ProsperoMgr, ps5-homebrew-dev-protocol, kstuff-lite,
SDL, MihawkOS) plus the session-1 logs. Line numbers are the clones as they are
on disk; URLs are given for the public form.

## Decision (read this first)

1. **Our app ships as a native PS5 title**, never as a payload: an fSELF
   `eboot.bin` + `sce_module/libc.prx` + `sce_sys/param.json` in a folder under
   `/data/homebrew/PPSA#####`, registered by ShadowMountPlus and launched from
   the home screen (or by `sceSystemServiceLaunchApp`).
2. Direct memory is a **property of the title process**, not of the code: the
   system gives a title the application pool (11.4-12 GiB, read it at runtime).
   RADV's PS5 winsys then has a real heap, and every Vulkan allocation is a
   direct-memory allocation mapped for the CPU and the GPU at one address.
3. A payload cannot get that pool from its own budget. It can borrow from one:
   `sceKernelAllocateMainDirectMemory` (~64 MiB proven) or
   `sceKernelAllocateDirectMemoryForApp` (GiBs, but only while a title owns the
   pool and the payload is its child). That is a helper trick, not an app
   platform — and with no title running, the pool is 0.

The 0-heap failure of session 1 is therefore **not a RADV bug and not a missing
env var**: it is the payload's zero direct-memory budget showing through. See
`$SPLAT_DATA_DIR/runs/console_session_1/SUMMARY.md` (step 2/3) and here §1.

## 1. Why the payload saw 0 bytes, and why RADV then had 0 heaps

- elfldr starts every payload as a child with **budget 0**:
  `sys_budget_set(0)` in `elfldr_rfork_entry` before it execs `SceSpZeroConf`
  (`ref/elfldr/elfldr.c:564-575`; same code in
  [ps5-payload-dev/elfldr](https://github.com/ps5-payload-dev/elfldr/blob/master/elfldr.c)).
  The payload is a descendant of `ProsperoMgr.elf` -> `elfldr.elf`, neither of
  which ever had an application pool.
- Consequence, measured by PS5LM's own memprobe on 13.60 and by us:
  `GetDirectMemorySize() = 0`, `AvailableDirectMemorySize` -> `0x8002000c`,
  `AllocateDirectMemory` -> `0x80020023` (EMPTY/EAGAIN), flexible pool
  8.00 GiB configured / 7.97 GiB free, malloc ceiling ~6.25 GiB
  (`ref/PS5LM/docs/CONSOLE.md:18-25`, session-1 `02_probe_mem.log:17-21`).
- RADV's heap **is** that number, nothing else:
  `radv_ps5_memory_pool_bytes()` returns `sceKernelGetDirectMemorySize()`
  (`ref/PS5_Mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c:271-277`), the
  winsys puts it in `drm_amdgpu_memory_info.vram/cpu_accessible_vram`
  (`radv_ps5_winsys.c:203-206`), `ac_fill_memory_info` turns it into
  `vram_size_kb`, and `radv_physical_device_init_mem_types` only creates a heap
  `if (visible_vram_size)` (`ref/PS5_Mesa/src/amd/vulkan/radv_physical_device.c:505-566`).
  Zero in -> `memoryHeapCount = 0`, `memoryTypeCount = 0`; `vkCreateDevice`
  still succeeds, the first `vkAllocateMemory` can never be legal. That is
  exactly the session-1 log (`03_vk_hello.log:111-115`).
- Every BO is one direct allocation, CPU and GPU at the same address:
  `ref/PS5_Mesa/src/amd/vulkan/winsys/ps5/radv_ps5_bo.c:14-27` (comment and
  `buffer_create`), `.c:290-345` (`sceKernelAllocateDirectMemory(... type 12)`
  then `sceKernelMapDirectMemory(... CPU_RW|GPU_RW)`), protection bits and type
  at `radv_ps5_platform.c:204-209`. With no pool, `buffer_create` fails with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` for everything RADV needs, including its own
  command/descriptor/shader memory. A device with 0 heaps therefore cannot be
  rescued by importing host memory: RADV's own allocations fail first.

## 2. How PS5LM gets its budget and where its GPU memory lives

**It is a title, not a payload.** PS5LM today is `ps5/app` (PPSA99575), an
fSELF `eboot.bin` linked with RADV, packaged with `sce_module/libc.prx` and
`sce_sys/param.json`, installed as a folder under `/data/homebrew/PPSA99575`
and registered by ShadowMountPlus (`ref/PS5LM/README.md:96-103`,
`docs/BUILDING.md:12-16`). The original *payload* PS5LM is archived and capped
at 1.6 GB, "bigger models need a native app" (`docs/PAYLOAD.md:10,32`).

**Budget.** The system gives the title the application pool; PS5LM does not
allocate it. Measured on the project console: "Direct memory for the title
12 GiB; RADV reports one 11.44 GiB device heap with
`radv_enable_unified_heap_on_apu` (8 GiB without)" (`docs/CONSOLE.md:85`). The
app reads it, never assumes it: `sceKernelGetDirectMemorySize()` +
`sceKernelAvailableDirectMemorySize` in `log_memory()`
(`ref/PS5LM/ps5/app/main.cpp:347`), and it sets the unified-heap knob before
the GPU opens (`main.cpp:636`).

**Where GPU memory is allocated.** In RADV's PS5 winsys, per BO, as above:
one `sceKernelAllocateDirectMemory` of type 12 + GPU mapping per buffer, GPU VA
== CPU VA (`PS5_Mesa` `radv_ps5_bo.c:14-113`). PS5LM adds no allocator of its
own for the GPU; what it adds is a **CPU heap in the same direct pool**
(`libps5platform`: `--wrap=malloc/calloc/realloc/free/...` with dlmalloc
mspaces over a 16 GiB reserved range, `ref/PS5_PayloadSDK/platform/include/ps5platform/heap.h:1-45`
and `platform/src/heap.c`), because a title's libc heap runs out long before
the app does (the flexible budget behind it is 448 MiB). `scripts/build-app.sh:196-225`
is the whole title link/convert/sign/package recipe.

**Launch, exit.** Launched by the shell or by a tiny "launch controller" payload
calling `sceSystemServiceLaunchApp`; a title may not `exit()` (`exit`/`_Exit`
end in SIGSYS) and stops through `sceSystemServiceLoadExec("exit", nullptr)` or
a self-restart via `LoadExec("/app0/eboot.bin")` (`docs/CONSOLE.md:93`,
`ps5/app/main.cpp:279-290,968`).

## 3. How PS5VR gets VideoOut-registrable memory

Also a **title** (PPSA99177), plus a resident **helper payload** that gives the
title the real root (`/data`, USB) and later the headset cameras
(`ref/PS5VR/README.md:76-80`, `helper/ps5vr-helper.c:1-20,89-105,111-139`).

PS5VR does not use Vulkan for its video path; it uses AGC directly, and it
allocates its own GPU-visible memory:

- `direct_alloc(size, align)` = `sceKernelAllocateDirectMemory(0, 16 GiB, size,
  align, 12, &off)` + `sceKernelMapDirectMemory(&va, size, 0x33, 0, off, align)`
  (`app/src/app_vr.c:242-255`). Type 12, CPU+GPU read/write, 2 MiB aligned.
- The main pool: `eng_agc_runtime_init` allocates one ~300 MiB block the same
  way and subdivides it into 2x 64 MiB scanout surfaces, a transient ring,
  DCBs, fences, composite/depth/stencil/layer surfaces
  (`app/engine/media/src/eng_agc_runtime.c:37-39,111-119,1071-1100`); those CPU
  VAs are what `sceVideoOutRegisterBuffers2` gets
  (`eng_agc_runtime.c:1476-1481`, attribute set by
  `sceVideoOutSetBufferAttribute2`).
- PSVR2: `sceVideoOutOpen(0xff, 32, 0, NULL)` (bus 32 = VR), eye buffers from
  another direct allocation (`vr_direct_alloc`, `eng_agc_runtime.c:2115-2132`,
  `eng_agc_vr_targets` at `:2146-2188`), turned into AGC texture descriptors and
  submitted as a **type 0x10 layer** through `sceVideoOutRegisterBuffers2` with
  the VR attribute (`app/src/app_vr.c:825-849`).
- Its own memory figures: direct 12288 MB with an 11 GB contiguous free run,
  flexible **448 MB** (`app/engine/media/include/eng_direct_mem.h:66-80`), which
  is why it also replaces `malloc` and spills large blocks into direct memory
  (`app/runtime/malloc_shim.c:41-70,136-173,218-240`).

So "VideoOut-registrable" means: a **direct-memory** allocation, 2 MiB aligned,
mapped for the GPU, whose CPU VA is passed to `sceVideoOutRegisterBuffers2`.
There is no separate VideoOut allocator.

| | PS5LM | PS5VR |
| --- | --- | --- |
| Process | title PPSA99575 | title PPSA99177 + helper payload |
| GPU API | RADV (Vulkan) | AGC + VideoOut + Hmd2 |
| GPU memory | RADV winsys, per-BO direct alloc type 12 | own direct allocs (12 for scanout/VR, 3 for CPU staging) |
| Budget | title app pool (11.44 GiB heap) | title app pool (12 GiB) |
| CPU heap | direct memory via `libps5platform` | direct memory via `malloc_shim.c` |
| Logs | `/data/PS5LM/app.log` (+ klog) | klog via `sceKernelDebugOutText` + USB file |

## 4. Can a payload get a direct-memory budget?

Three things work or are worth testing, and one does not.

**(a) `sceKernelAllocateMainDirectMemory` — works in a payload, small.**
This is the "main direct" pool that ps5-payload-dev's SDL uses for its 64 MiB of
VideoOut buffers, mapped `0x33` and registered exactly like PS5VR's scanout
(`ref/SDL/src/video/ps5/SDL_ps5video.c:207-243`). Hardware evidence: a payload
alongside a running title allocated 1 MiB and 64 MiB with it and touched them;
a 2 GiB request was refused (`https://github.com/mihawk-99/PS5_MihawkOS/blob/main/evidence/payload-direct-1m-2026-10-07-01.json`,
`payload-direct-64m-…`, `payload-direct-2g-…`). The ceiling is not published;
PS5LM's memprobe climbs to 512 MiB (`ref/PS5LM/probes/memprobe/main.c:221-249`).
Too small for our 8 GiB conversion.

**(b) `sceKernelAllocateDirectMemoryForApp` — works when an app pool exists.**
The SDK fork documents it: "A payload has no direct memory budget of its own,
so `sceKernelAllocateDirectMemory` is refused there; this takes the pages from
the running application's pool instead"
(`ref/PS5_PayloadSDK/platform/include/ps5platform/kernel.h:44-49`,
`platform/src/placement.c:27-40`). Hardware evidence (a jailbroken console with
a native title running): a payload holding budget 0 allocated **2 GiB** with it,
touched and released it (`payload-direct-forapp-2g-2026-10-07-01.json`), and the
same payload's plain `AllocateDirectMemory` was refused `0x80020023` — the same
negative control we measured. The page survived the owner title exiting
(`payload-direct-forapp-owner-end-…`). No kernel budget patch is involved:
"Payload application allocator | Shared application-pool backing passed without
a kernel budget patch" and "Helpers retain budget 0, with no budget reassignment
or kernel data patch"
(`PS5_MihawkOS/docs/CHILD_PROCESSES.md:17`,
`docs/WINDOWS_PROCESSES.md:85-104`).

Caveat for us: that test ran with a title owning the pool, and the payload was
started by that title's own loader — in it, `sceKernelGetDirectMemorySize()`
reported 11712 MiB, which suggests the reported pool follows the **parent
process**, not the payload binary. In session 1 no big app was running,
`GetDirectMemorySize()` was 0, and our probe never tried `ForApp`. **Unknown:
does the home screen alone expose a lendable pool?** A 64 MiB probe would answer
it in one 10 s payload run. Even if it does, borrowing the home screen's pool is
exactly how PS5LM's payload froze the UI at 2.7 GB locked (`docs/CONSOLE.md:62`):
not a shipping route.

**(c) Let the system spawn our title.** A payload (or ProsperoMgr) calls
`sceUserServiceGetForegroundUser` + `sceSystemServiceLaunchApp(title_id, argv,
ctx)` and the work happens in a process with a real budget. This is the route
we ship; the payload only launches.

**(d) Kernel patching of the budget — not available, not sensible.**
`sys_budget_set` takes a **budget id**, not bytes, and requires a system
capability (`https://github.com/RPCSX/rpcsx/blob/master/kernel/orbis/src/sys/sys_sce.cpp:869-884`);
elfldr's `0` is "the budget of a system app" (`PS5LM/docs/CONSOLE.md:25`).
kstuff-lite patches FSELF/crypto/loader, no budget table (`ref/kstuff-lite/`,
grep: only a PS4-debug leftover `ps4_xchg_budget` in `gdb_stub/`); etaHEN's
jailbreak request changes rights (uid, sandbox, authid), not the pool
(`PS5LM/docs/CONSOLE.md:25`). Writing kernel structures to fake a title-sized
budget would still have to take the pages from the same physical pool, with no
accounting, and it is the class of change that has already cost consoles: a
bare elevated-title `rfork_thread` caused a user-confirmed kernel panic
(`PS5_MihawkOS/docs/CHILD_PROCESSES.md:19-21`).

Risks, ranked: (1) a payload big enough to matter starves the home screen
(freeze/shutdown, measured by PS5LM); (2) killing/spawning titles from a payload
is the fragile path (PS5LM: killing a rendering title "has been followed by lost
consoles"; the dev protocol saw a kernel panic after many kill cycles,
`ref/ps5-homebrew-dev-protocol/docs/CONTROLLERS.md:73-95`); (3) kernel patching
is unsupported, unreproducible and outside what a public homebrew app may ask of
its users.

## 5. Packaging and installing a homebrew title on 13.60

Required on the console (session 1 showed kstuff, ShadowMountPlus, elfldr,
ProsperoMgr and klogsrv running; no etaHEN process was visible, so the
HEN/allowlist item below is the one to verify): **kstuff(-lite)** for fSELF,
**etaHEN/OnionHEN**, **ShadowMountPlus**
(1.7beta2 is what PS5LM ships against; the FW 12+ TitleDir bridge is its default
per-title path), elfldr + klogsrv for development, ftpsrv optional
(`PS5LM/README.md:96-103`). "GarlicMgr" on port
8082 is `garlic-savemgr` (a save manager), not a title installer — it plays no
part in installing our app.

Y2JB (the YouTube-app userland exploit) is only the entry: it is how kstuff, the
HEN and ProsperoMgr get started after a reboot (`ref/Y2JB/README.md:1-8`). It has
no role in packaging or launching our title; it just has to have run.

**Folder layout** (what ShadowMountPlus scans, `ShadowMountPlus/README.md:386-392`;
what PS5LM/PS5VR ship):

```
/data/homebrew/PPSA#####/
  eboot.bin                 # fSELF (signed container, fake magic 0x1D3D154F)
  sce_module/libc.prx       # required C-library companion module
  sce_sys/param.json        # titleId, contentId, category, amm/kernel sizes
  sce_sys/icon0.png         # optional but wanted (home tile)
  sce_sys/pic0.dds, pic1.dds# optional backgrounds
  <our assets>              # read as /app0/<...>
```

**Title ID.** Four letters + five digits, and a `contentId` that matches it. Pick
one in the PPSA99xxx homebrew range that is not in `GET /api/titles` and not used
by PS5LM (PPSA99575), PS5VR (PPSA99177) or PS5_Vulkan (PPSA99014/99170); a
collision reuses that title's app.db entry and sandbox. A `PPSA*` id also gets
ShadowMountPlus's writable `/data` mount (below).

`param.json` essentials, from PS5LM's (`ref/PS5LM/ps5/app/sce_sys/param.json`):
`titleId` `PPSA#####`, `contentId` `IV0000-PPSA#####_00-<16 chars>` (36 chars),
`conceptId` = the 5 digits, `applicationCategoryType: 0`,
`applicationDrmType: "free"`, `localizedParameters.<lang>.titleName`, and the
memory/VA block that lets RADV reserve its 256 GiB device region:

```json
"amm": { "multimapVaRangeInGib": 512, "pagetableMemorySizeInMib": 256, "vaRangeInGib": 512 },
"kernel": { "cpuPageTableSize": 268435456, "gpuPageTableSize": 268435456 }
```

PS5_Vulkan's smoke title carries no block and PS5VR's has none either, but
PS5LM and ProsperoAI both do, and RADV's device region is a
`sceKernelReserveVirtualRange` of 256 GiB at `0x40_0000_0000`
(`PS5_Mesa/.../radv_ps5_platform.c:150-151,246-262`): include it, it costs
nothing and removes a class of "buffers land where the kernel wants" failures.

**Install = the folder appears.** ShadowMountPlus scans `/data/homebrew` every
15 s (`scan_interval_seconds`, `stability_wait_seconds=10`), verifies
`eboot.bin`+`sce_sys/param.json`, copies `sce_sys` metadata to
`/user/appmeta/<TID>`, nullfs-mounts the folder at `/system_ex/app/<TID>`, writes
`/user/app/<TID>/mount.lnk`, then registers the title: on FW 12+ by arming its
SceShellCore **TitleDir bridge** and dispatching one `sceAppInstUtilAppInstallAll`
into the internal TitleDir path (`src/sm_install.c:225-390`,
`src/sm_filesystem.c:1327-1366`, `src/sm_shellcore_bridge.S:251-258`).
"Register failed: TitleDir bridge unavailable" means ShadowMountPlus's hook is
stale: reload the payload (`PS5LM/docs/CONSOLE.md:107`). A forced rescan is
available: `POST /api/v1/scan` on ShadowMountPlus's own API (port 10101),
`docs/api.md`.

**Launch remotely** — ProsperoMgr v1.1 can do it, no controller and no 9021:

```
GET  /api/titles                   -> {"titles":[{"title_id":...}, ...]}
POST /api/titles/launch  {"title_id":"PPSA#####"}
```

`api_routes.cpp:584-592` -> `LaunchTitle()` -> `sceUserServiceGetForegroundUser`
then `sceSystemServiceLaunchApp(title_id, nullptr, &ctx)`
(`ref/ProsperoMgr/source/backend/src/titles.cpp:580-604`). It needs a
foreground user and a registered title; an unregistered title fails. Do not
launch a title "some other way without a user": the system ends it after ~20 s
(`PS5LM/docs/BUILDING.md:79-81`). The equivalent in-payload code (if we ever
want our own launcher) is `ref/ps5-homebrew-dev-protocol/scripts/controllers/launch.c:24-45`.

**Logs from a title.** A title has no stdout reader (elfldr's `/dev/deci_stdout`
belongs to payloads). Two sinks, both proven in titles:

- **klog**: `sceKernelDebugOutText(0, "ps5splat: ...\n")` -> `/dev/klog` ->
  klogsrv :3232, live, before VideoOut is up (`PS5VR/app/engine/include/eng_boot_trace.h:31,47-58`;
  PS5VR's loader/klog lines are how its bring-up was debugged). Our line tag
  must identify the app; the kernel prefixes the process name.
- **file**: `freopen("/data/ps5splat/app.log", ...)` and read it back with
  `GET /api/ftp/download?path=/data/ps5splat/app.log` (ProsperoMgr,
  `api_routes.cpp:1532`) or FTP. PS5LM does this
  (`ps5/app/main.cpp:103,620-630`).

**`/data` from a title.** ShadowMountPlus mounts `/data` writable (nullfs) for
PS5 `PPSA*`, PS4 `CUSA*` and homebrew titles (`ShadowMountPlus/README.md:206`).
PS5LM still asks the HEN because OnionHEN needs the title on its allowlist:
`PPSA99575` in `[app_jailbreak] exact_title_ids` of
`/data/OnionHEN/config.ini` (`PS5LM/README.md:114-115`,
`ps5/app/main.cpp:237-277`). PS5VR goes further: a helper payload sets the
process's rootdir/jaildir to the real root after startup
(`helper/ps5vr-helper.c:89-105`). Plan: try ShadowMountPlus first, keep the HEN
allowlist as the documented fallback, and only then consider a helper.

## 6. Our build: payload ELF vs title

| | today (`platform/ps5/vk_hello`) | title (new target) |
| --- | --- | --- |
| Output | `*.elf`, uploaded by elfldr | `eboot.bin` fSELF in a folder |
| Start | SDK `crt1.o`, `main(argc,argv)` | `_start(process_parameters, loader_teardown)` from `tooling/native/app_crt.cpp`, `-e _start` |
| Link | payload recipe (`platform/ps5/radv-link.mk`) | RADV title recipe: `ref/PS5_Vulkan/tools/radv-link.sh` flags + `tooling/psbc/ps5-pie-unwind.ld` + `--version-script tooling/native/app-symbols.map --exclude-libs=ALL`, then `--as-needed <sdk>/target/lib/*.so` |
| Convert | — | `ps5-native-tool link --in llvm-pie.elf --out eboot.elf --stub-dir <sdk>/target/lib --stub libSceAgc.so --stub libSceAgcDriver.so --module-sdk 0x02000009 --companion-sdk 0x08050001` |
| Sign | — | `ps5-native-tool self --sign --in eboot.elf --out eboot.bin --magic 0x1D3D154F` |
| Runtime | none | `sce_module/libc.prx` (SHA-256 `e6ff45d1…036`), `sce_sys/param.json`, icon |
| Exit | `exit()` is fine | never `exit()`; `sceSystemServiceLoadExec("exit", NULL)` or a self-restart |
| Log | stdout + klog | `sceKernelDebugOutText` + file |

Hosts: the title can be built **on this Mac**. PS5VR ships a macOS arm64
`ps5-native-tool` built from the same builder (`ref/PS5VR/app/tools/native-mac/ps5-native-tool`,
`app/scripts/build-native-mac.sh:37-45`), and the SDK fork (with
`libps5platform.a`) is already in `ref/radv-ps5/sdk`. The `libc.prx` is
byte-identical to PS5_Vulkan's published artifact
(`ref/PS5_Vulkan/runtime/libc.prx.sha256`), so it can be verified, not trusted.
Treat the Mac binary as a **dev-loop tool**; for releases build the converter and
`libc.prx` from source (PS5_Vulkan `make` / `tools/rebuild-libc.sh`) so the
artifact is reproducible from the repo. Fallback host: PS5_Vulkan's
`tools/build-radv-title.sh` in the Linux/Docker toolchain we already use for
RADV.

**App-code changes** (all found by PS5LM/PS5VR crashing, so we do them up front):

- Hide the splash: `sceSystemServiceHideSplashScreen()`.
- `HOME`/cache paths must be set explicitly; there is no `$HOME`.
- ACO's shader cache goes to `/app0/radv-shader-cache` (the winsys sets
  `MESA_SHADER_CACHE_DIR` itself, `radv_ps5_platform.c:246-252`); keep that
  directory across deploys — first-compile time is a real cost.
- Some libc imports resolve to NULL in a title (`getaddrinfo`, `readlink`,
  `mkstemp`, `isatty`, `fork`...). PS5LM's `ps5/app/compat_app.c` and PS5VR's
  `libc_ext.c` are the catalogue; PS5VR's build even relinks with a generated
  `import_check.c` that reports every NULL import (`app/scripts/build.sh:185-203`).
  Do the same: it turns a jump-to-0 crash into a log line.
- Keep large allocations in direct memory via `libps5platform`'s wraps; the
  title's malloc heap is 448 MiB of flexible memory.

## 7. Recommended dev loop

One-time: build the title folder (`make -C platform/ps5/title`), produce
`build/ps5/title/PPSA#####/{eboot.bin,sce_module/libc.prx,sce_sys/…}`.

Per iteration (close -> deploy -> register -> launch -> klog -> stop):

```sh
T=PPSA#####; H=$PS5_HOST
# 0. stop the previous run first (never overwrite a running eboot.bin)
curl -sS -X POST "http://$H:7070/api/ftp/upload?path=/data/ps5splat/quit&overwrite=1" \
     --data-binary 'quit'
#    (the title polls it and calls sceSystemServiceLoadExec("exit", NULL);
#     last resort: GET /api/processes, then POST /api/processes/kill {"pid":N})
# 1. make the folders, then upload (ProsperoMgr, any path, 256 GiB limit)
for d in "" /sce_sys /sce_module; do
  curl -sS -X POST http://$H:7070/api/ftp/mkdir -d "{\"path\":\"/data/homebrew/$T$d\"}"
done
curl -sS -X POST "http://$H:7070/api/ftp/upload?path=/data/homebrew/$T/eboot.bin&overwrite=1" \
     --data-binary @build/ps5/title/$T/eboot.bin
# … every other file the same way; or zip the folder and POST /api/ftp/unzip
# 2. ask ShadowMountPlus to rescan now (port 10101), then wait for the title
curl -sS -X POST http://$H:10101/api/v1/scan -H 'Content-Type: application/json' -d '{}'
until curl -sS http://$H:7070/api/titles | grep -q $T; do sleep 2; done  # add a timeout
# 3. launch (foreground user required), with klog already streaming
platform/ps5/scripts/klog.sh            # terminal 1
curl -sS -X POST http://$H:7070/api/titles/launch -d "{\"title_id\":\"$T\"}"
# 4. stop with step 0 when done; confirm via GET /api/titles/running
```

Success evidence, per the dev protocol: klog `launchApp(PPSA#####)` then
`EXEC /app0/eboot.bin`, and ShadowMountPlus `[GAME] started: PPSA#####`; on stop,
`[LINK] runtime layers released: PPSA#####`
(`ref/ps5-homebrew-dev-protocol/docs/CONTROLLERS.md:73-83`). Build this once as
`platform/ps5/scripts/run-title.sh` (close/deploy/scan/launch/klog/stop) next to
`run.sh`, and keep `run.sh` for payload probes and helpers.

**First console gate** (before porting anything): a hello title whose `main`
prints `sceKernelGetDirectMemorySize`, `sceKernelConfiguredFlexibleMemorySize`,
then runs `vk_hello`'s existing device/heap log and one compute dispatch. Pass
criteria: `memory: 1 heaps` with roughly 11.4-12 GiB device-local, a host-visible
type, and the fill verified. Only then wire SHARP/Hmd2.

## 8. Can the RADV winsys fall back to the flexible pool?

**No, not as a heap, and not for our sizes.** The pieces:

- The winsys has exactly one source of heap size (direct memory) and never
  queries flexible memory (`radv_ps5_platform.c:272-289`,
  `radv_ps5_winsys.c:203-206`).
- A BO is not "some mapping": it is a direct-memory allocation with a physical
  offset, mapped CPU+GPU at one address, and the offset is needed to release it,
  to re-map it at a fixed address (placed mappings), and for sparse binds
  (`radv_ps5_bo.c:14-27,120-160`; `radv_ps5_memory_alloc*` at
  `radv_ps5_platform.c:290-345,431-520`). Flexible memory has no physical offset
  and no `sceKernelReleaseDirectMemory`; a flexible-backed BO would be a new
  allocator, not a fallback.
- GPU visibility itself is not the blocker: a title's anonymous/flexible memory
  takes GPU read/write through `sceKernelMprotect(… CPU|GPU …)`, and a compute
  shader wrote it through the CPU address (`PS5_Vulkan/docs/HARDWARE_FINDINGS.md`,
  2026-09-27 section; probe `radv/radv_smoke.c:2865-2960`). RADV already models
  that as `VK_EXT_external_memory_host` import
  (`radv_ps5_bo.c:165-207` -> `radv_ps5_memory_grant_gpu`,
  `radv_ps5_platform.c:756-763`). But it is per-allocation, app-driven, and
  limited to whole pages; it does not create a heap, and command buffers,
  descriptors and shader arenas would still have to come from somewhere.
- Placement rules make a flexible heap nearly unusable anyway: everything the
  GPU reaches must be below 2^47, descriptor-reachable ("32-bit") buffers must
  live inside the 4 GiB window at `0x2_0000_0000` (`radv_ps5_platform.h:18-33`),
  and the device region is a 256 GiB reservation at `0x40_0000_0000`. The kernel
  places flexible memory wherever it likes; RADV cannot honour its own 32-bit
  window with it.
- Sizes are wrong for us in both worlds: a title's flexible pool is **448 MiB
  configured / 403 MiB free** (`PS5_PayloadSDK/platform/docs/PROBE.md:20-24`;
  PS5VR's malloc shim exists because of exactly this limit), and a payload's
  7.97 GiB is shared with the system with a ~6 GiB malloc ceiling (session-1
  log; `PS5LM/docs/CONSOLE.md:18-21`), where PS5LM froze the home screen at
  2.7 GB (`PS5LM/docs/CONSOLE.md:62`). Our spec needs ~8 GiB for SHARP.

The **only** cheap payload-side experiment worth keeping is different: teach the
winsys a fallback to `sceKernelAllocateDirectMemoryForApp` plus a pool size from
an env var, to prove the mechanism in a payload. It still borrows the running
app's pool (no ownership, no accounting, starves the UI) and the device heap
would be sized by a guess, so it is a diagnostic, not a design. Do it only if
the title route is blocked.

## 9. What this asks of the user, and what it costs

- Console: ShadowMountPlus loaded after kstuff/etaHEN (it is what turns the folder
  into a home-screen tile), elfldr through ProsperoMgr, klogsrv for logs. Nothing
  new to install for us; the app is a folder copy like PS5LM/PS5VR.
- One manual step at most: if the first title run cannot write `/data`, add our
  title ID to the HEN's app-jailbreak allowlist (OnionHEN: `exact_title_ids`) and
  start the app again. PS5LM documents exactly this (`README.md:114-115`).
- Costs of being a title: it must never `exit()`, it must be closed gracefully
  (a quit file) before a redeploy, and a forced kill is a last resort. That is the
  same contract PS5LM/PS5VR live with.
- A title can also download over HTTPS with `libSceHttp2`/`libSceSsl`
  (`PS5LM/docs/BUILDING.md:24`), which a payload cannot
  (`PS5LM/docs/CONSOLE.md:66`: `sceHttp2SendRequest` fails in a payload). The
  first-run model fetch therefore gets easier on the title route, not harder.
- A foreground user must be signed in: `sceSystemServiceLaunchApp` takes that
  user's id, and an app started without one is ended after ~20 s.

## 10. What is still unknown (cheap to answer)

1. Does the home screen alone expose a lendable app pool for
   `AllocateDirectMemoryForApp`? (probe: 64 MiB ForApp + MainDirect ladder,
   `sceKernelGetBudget`, `mprotect` GPU on flexible; ~10 s payload.)
2. How large is a payload's MainDirect pool? (64 MiB proven; memprobe's 512 MiB
   ladder in `ref/PS5LM/probes/memprobe/main.c:221-249`.)
3. Does 13.60 report 12288 or 11712 MiB to a title? (PS5LM's console: 11.44 GiB
   heap; PS5_PayloadSDK's probe console: 12 GiB.) Our hello title prints the number.
4. Does ShadowMountPlus's `/data` nullfs suffice for a title, or do we need the
   HEN allowlist / a helper payload? Try a write probe on first launch.
5. Is `sceKernelMprotect` with GPU bits allowed in a **payload**? (title: proven;
   payload: untested — only matters for the fallback in §8.)

## Sources

Local (clones under `$SPLAT_DATA_DIR/ref/`): `PS5LM` (`docs/CONSOLE.md`,
`docs/PAYLOAD.md`, `docs/BUILDING.md`, `ps5/app/main.cpp`, `ps5/app/sce_sys/param.json`,
`scripts/build-app.sh`, `probes/memprobe/main.c`); `PS5VR` (`README.md`,
`app/src/app_vr.c`, `app/engine/media/src/eng_agc_runtime.c`,
`app/engine/media/include/eng_direct_mem.h`, `app/runtime/malloc_shim.c`,
`helper/ps5vr-helper.c`, `app/scripts/build.sh`); `PS5_Mesa`
(`src/amd/vulkan/winsys/ps5/*`, `src/amd/vulkan/radv_physical_device.c`);
`PS5_Vulkan` (`docs/HARDWARE_FINDINGS.md`, `radv/radv_smoke.c`,
`tools/radv-link.sh`, `tools/build-radv-title.sh`, `tooling/native/app_crt.cpp`,
`runtime/README.md`); `PS5_PayloadSDK` (`platform/include/ps5platform/kernel.h`,
`platform/src/placement.c`, `platform/docs/PROBE.md`); `elfldr/elfldr.c`;
`ShadowMountPlus` (`README.md`, `src/sm_install.c`, `src/sm_filesystem.c`,
`src/sm_shellcore_bridge.S`, `docs/api.md`); `ProsperoMgr`
(`source/backend/src/api_routes.cpp`, `titles.cpp`); `ps5-homebrew-dev-protocol`
(`scripts/controllers/launch.c`, `close.c`, `docs/CONTROLLERS.md`); `SDL/src/video/ps5/SDL_ps5video.c`;
`kstuff-lite`; `garlic-savemgr`.

Web: [PS5LM](https://github.com/cobanov/PS5LM),
[PS5VR](https://github.com/tertiumndatur/PS5VR),
[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa),
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan),
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK),
[PS5_MihawkOS payload memory evidence](https://github.com/mihawk-99/PS5_MihawkOS/tree/main/evidence)
(`payload-direct-*.json`, `docs/WINDOWS_PROCESSES.md`, `docs/CHILD_PROCESSES.md`),
[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus),
[ps5-homebrew-dev-protocol](https://github.com/manoma080/ps5-homebrew-dev-protocol),
[ps5-payload-dev/elfldr](https://github.com/ps5-payload-dev/elfldr),
[ps5-payload-dev/SDL](https://github.com/ps5-payload-dev/SDL),
[RPCSX `sys_budget_set`](https://github.com/RPCSX/rpcsx/blob/master/kernel/orbis/src/sys/sys_sce.cpp).

Session evidence: `$SPLAT_DATA_DIR/runs/console_session_1/{SUMMARY.md,02_probe_mem.log,03_vk_hello.log}`.

## 11. Task T1: the title package built, vk_hello as a title (build only)

2026-10-26, console off. §7's "one-time" step is now real:
`make -C platform/ps5/title` builds `build/ps5/title/PPSA99200/` on this Mac in
a few seconds (3.5 s from an empty `build/ps5/title`), and
`platform/ps5/scripts/title.sh install|launch|stop|status`
carries out §7's per-iteration loop. Nothing was run on the console: every
statement below is a build result, a static check, or a test against a local
mock.

**The package** (gitignored, `build/ps5/title/PPSA99200/`):

| File | Bytes | Check |
| --- | --- | --- |
| `eboot.bin` | 25,729,767 | `self --inspect`: signed, plaintext, 12 segments, integrity valid; the linked ELF is 24,802,720 |
| `sce_module/libc.prx` | 1,284,674 | SHA-256 `e6ff45d1…0036` = the published artifact, verified by the build |
| `sce_sys/icon0.png` | 3,911 | 512x512 RGB, generated by `platform/ps5/title/gen-icon.py` |
| `sce_sys/param.json` | 1,046 | provisional `PPSA99200` |
| folder | 27,025,408 | |

Static shape, read with LLVM 21's `readelf`: `NEEDED` = `libSceAgc.prx`,
`libSceAgcDriver.prx`, `libSceLibcInternal.prx`, `libSceSystemService.prx`,
`libSceVideoOut.prx`, `libkernel.prx`; entry `0x758DD0` = `_start` from
`app_crt.cpp` (`main` at `0x7590D0`); **300 undefined imports, all NID-encoded**
(`+0EDo7YzcoU#F#G` form: the converter rewrote every name), and **no exported
dynamic symbol** (the converter refuses exports, and the generated
`title-local.map` keeps every bound libc name local). The payload `vk_hello.elf`
still builds and still passes `verify-elf.sh`.

**Title ID.** `PPSA99200`, content `IV0000-PPSA99200_00-PS5SPLATVR000001`,
title "Photo Splat VR" — provisional, the user picks the final one (§5's open
question). Not in the research's used set (`PPSA99575`, `PPSA99581`,
`PPSA99177`, `PPSA99176`, `PPSA99014`, `PPSA99015`, `PPSA99170`, `PPSA99988`),
which `title.sh` now refuses by name. `param.json` carries §5's `amm` and
`kernel` block (512 GiB VA range, 256 MiB page tables), `applicationCategoryType:
0`, `applicationDrmType: "free"` and `conceptId` = the five digits.

**What the title build had to get right**, beyond §6's table:

- the link must be `-L $sdk/target/lib`: the SDK's `libc++.a`/`libc++abi.a`/
  `libunwind.a` name `pthread` as a **dependent library**, and lld resolves that
  through `-L` only (`target/lib/libpthread.a`); without it the link stops with
  "unable to find library from dependent library specifier: pthread";
- lld is driven directly, as PS5LM/PS5_Vulkan do, so `radv-link.mk`'s
  compiler-driver spellings have to be rewritten (`-Wl,--wrap,x` →
  `--wrap=x`, `-Wl,--defsym,…` → `--defsym=…`, `-Wl,--start-group` →
  `--start-group`) and `-nodefaultlibs` dropped;
- the platform's extra libc names that only some SDK pins define
  (`localeconv`, `strcasestr`, `mkstemp`, `readlink`, `link`, `symlink`) are
  bound when `libps5platform.a` exports them, as `radv-link.sh` does: a title's
  import of `readlink`/`mkstemp` is NULL at run time and RADV calls both;
- `--exclude-libs=ALL` plus the generated local map is enough for the converter
  to accept the PIE (no `--version-script app-symbols.map` needed separately;
  `_Zn*`/`_Zd*` are in the generated map).

The tools stay outside the repo, like the RADV archive: `TITLE_TOOLING`
(`ref/PS5_Vulkan/tooling`: `app_crt.cpp`, `app_cpp_runtime.cpp`,
`psbc/ps5-pie-unwind.ld`, `native/ps5-pie.ld`), `PS5_NATIVE_TOOL`
(`ref/PS5VR/app/tools/native-mac/ps5-native-tool`, a dev-loop binary) and
`TITLE_LIBC_PRX` (`ref/PS5VR/app/tools/native-app/runtime/libc.prx`). A release
should build the converter and `libc.prx` from source; the hash check makes the
current substitution loud rather than silent.

**vk_hello as a title.** The same `main.cpp` with `-DPS5_TITLE`, so the title
and payload code paths cannot drift. Four differences, all forced by §5-6, 9:
it logs through `sceKernelDebugOutText` (same message text, so the runbook's
greps are shared), prints `direct pool`/`flexible pool` before the instance (the
number RADV will report as its heap), leaves RADV's `/app0/radv-shader-cache` in
place instead of disabling the cache, and ends with
`sceSystemServiceLoadExec("exit", nullptr)` instead of returning to the CRT. It
also calls `sceSystemServiceHideSplashScreen()` first.

**title.sh** (`platform/ps5/scripts/title.sh`): `install` = mkdir + one raw
upload per file under `/data/homebrew/<TITLE_ID>` through ProsperoMgr's file
API, size-verify, `POST /api/v1/scan` to ShadowMountPlus (10101), poll
`GET /api/titles` (90 s); `launch` = `POST /api/titles/launch` after checking
registration and that no other title runs, then poll `/api/titles/running`;
`stop` = write `/data/ps5-splat-vr/title-quit` and wait, never a kill; `status`
= read-only. `--dry-run` prints every request and sends nothing (works with no
`PS5_HOST`); only the FTP mkdir/upload/list, titles and scan routes are used,
and every path is checked to be under our two directories before a request is
built.

Verified offline against a local mock of ProsperoMgr + ShadowMountPlus
(`$SPLAT_DATA_DIR/runs/T1/`, request log in `mockroot/requests.log`): install
uploaded 4 files, verified sizes, scanned and registered; launch and stop
tracked `/api/titles/running`; `status` matched every size; `install` while
running refused (exit 5); 66 requests, none to a delete/rename/chmod/shutdown
route. Negative cases: a reserved id, a folder name that disagrees with
`param.json`, and a package without `eboot.bin` all stop with exit 2 before any
request; a refused ProsperoMgr exits 3 with one line.

**Still unknown** (this task's console gate, `docs/console-runbook.md` §1):

1. Does ShadowMountPlus register `PPSA99200` from a plain folder copy on 13.60,
   and does the tile launch? (§5's TitleDir bridge.)
2. What does `sceKernelGetDirectMemorySize()` report to a title here: 11712 or
   12288 MiB, and is the RADV heap 11.44 GiB as PS5LM measured? (The new `direct
   pool` line answers question 3 of §10.)
3. Is `/data` writable from the title without the HEN allowlist? (§10 question
   4; the probe does not write, the next app does.)
4. Does `sceSystemServiceLoadExec("exit", nullptr)` end it cleanly, and does the
   home screen come back? (PS5LM's path, unmeasured here.)

Add one line per finding to `docs/BACKLOG.md` after that run; the bench payloads
still need converting to titles (runbook step 7 lists the three changes).
