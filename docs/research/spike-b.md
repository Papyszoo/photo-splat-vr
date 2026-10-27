# Spike B, step 1: a RADV payload for the PS5

Research and build record for Task B1, 2026-10-26. Host: macOS 26.5, arm64
(Apple silicon), Docker 29.5 (colima, `linux/arm64` VM with amd64 emulation).
Console off for the night: **nothing was run on it and nothing was installed.**
The deliverable is `platform/ps5/vk_hello/vk_hello.elf`, built and statically
checked, plus the one-command console checklist in section 7. The pre-console
review's three fixes (bounded teardown with a deliberate leak, the unified-heap
setting, `probe_agc` importing all three modules) are applied and both ELFs
rebuilt; sections 3.3, 4 and 7 describe the current binaries.

## 1. Route: source build, no usable prebuilt archive

Checked first, as required:

| Source | Releases / CI artifacts | Verdict |
| --- | --- | --- |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | no GitHub releases; its `tooling.yml` CI uploads the boilerplate demo title (`PPSA….zip`/`.ffpfsc`), never the RADV archive | build from source |
| [PS5LM](https://github.com/cobanov/PS5LM) | v0.9.0/v0.9.1 ship `ps5lm-app-*.zip`, a title folder for ShadowMountPlus, not the archive; its CI builds and discards RADV | not reusable |
| [PS5LM-Plus](https://github.com/abhi245y/PS5LM-Plus) | v0.8.x ship title folders, same | not reusable |
| [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) | no releases | — |

So route **B**: build `libvulkan_radeon.ps5.a` in an amd64 Debian container with
PS5_Vulkan's own `tools/build-radv.sh release`, which is the build PS5LM's
`docs/BUILDING.md` describes (PS5LM adds three app-specific Mesa patches; they
are not needed for a compute probe and are not applied). Build glue lives in
`platform/ps5/radv/`; results live outside the repo in
`$SPLAT_DATA_DIR/ref/radv-ps5/`.

## 2. Versions and build time

| Item | Value |
| --- | --- |
| PS5_Vulkan | `f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed` (2026-10-12) |
| PS5_Mesa | `1ba9874f478657ccf6da72ce55f0260164cc0bd0` (Mesa 26.2.0 + PS5 winsys) |
| PS5_PayloadSDK | `3e31e0b1b68ada28beca68adc480cf0c3d17b9df` (2026-10-24) |
| Mesa patches | none (PS5LM's `patches/mesa` are title/app-specific) |
| Build image | `debian:trixie-slim` (`sha256:62bd423a…`), linux/amd64 under Rosetta |
| Toolchain | Clang/LLVM 19.1.7, lld 19, meson/ninja, glslang, SPIRV-LLVM-Translator, libclc |
| Jobs / memory | `-j2`, 2 CPUs, 5 GiB container (`BUILD_JOBS=2`) |
| Build time, RADV release | **1101 s (18 m 21 s)**, final revision; first attempt at the older SDK pin 1322 s (22 m 02 s) |
| Image build | 2 m 06 s first time, layer-cached afterwards |
| Archive | `libvulkan_radeon.ps5.a`, 273,618,194 bytes, SHA-256 `f649baf253ca1ea552dd365cd620a078f9f7063c16854feafb2e7d82485b9823` |
| Clang builtins | `libclang_rt.builtins-x86_64.a`, SHA-256 `40f81d93515a4cc6d45563fd504ed5602e49633a76739fdd5852c107c24d2789` |
| Full record | `$SPLAT_DATA_DIR/ref/radv-ps5/BUILD.txt`, log in `runs/B1/radv-build.log` |

**Deviation, deliberate:** PS5_Vulkan's `tools/setup-native-dependencies.sh`
pins SDK fork `b83202b`, and PS5LM's CI pins the same pair. That revision
predates the libc shims RADV's link recipe now expects (`ps5_readlink`,
`ps5_mkstemp`, `ps5_localeconv`); PS5LM supplies equivalents through its
title's `libc.prx`/`compat_app.c`, but a payload has no such module. The archive
was therefore built against the fork's current revision `3e31e0b`, which adds
them (the header diff between the two is additive; `kernel.h` +10 lines,
`libc.h` +151). One SDK revision is in play: build and link use the same tree.

### 2.1 How it was built

```sh
platform/ps5/radv/build-in-docker.sh          # ~18 min, 2 CPUs, 5 GiB
```

The script:

1. builds/starts the image from `platform/ps5/radv/Dockerfile` (the package set
   of PS5LM's CI: LLVM 19, lld, meson/ninja, libclc, SPIRV-LLVM-Translator,
   glslang, glslc);
2. `docker cp`s the read-only clones of `PS5_Vulkan`, `PS5_Mesa` and
   `PS5_PayloadSDK` from `$SPLAT_DATA_DIR/ref` into the container. A bind mount
   from `/Volumes` is not usable: colima shares `/Users` and `/tmp`, not the
   external volume, and a non-shared bind source silently becomes an empty dir;
3. exports the SDK fork at the pinned revision with `platform/tools/setup-sdk.sh`,
   then runs `setup-native-dependencies.sh --skip-sdk` (zlib only);
4. runs `tools/build-radv.sh release` with `NINJA` wrapped to `-j2`;
5. copies `libvulkan_radeon.ps5.a`, its `include/`, the SDK and Clang's Linux
   builtins to `$SPLAT_DATA_DIR/ref/radv-ps5`, builds the two AGC import stubs
   (section 3.2) and writes `BUILD.txt`.

A re-run starts from a fresh copy (the image layers are cached, the Meson tree
is not); the script removes the container after a successful copy.
`RADV_REBUILD_IMAGE=1` rebuilds the image.

## 3. What the payload links

### 3.1 Link model (`platform/ps5/vk_hello/Makefile`)

No Vulkan loader. The archive is linked whole and every command comes from
RADV's `vk_icdGetInstanceProcAddr`, as PS5_Vulkan's `radv/radv_smoke.c` does for
a title. The link inputs are RADV's title recipe (`tools/radv-link.sh`) with the
payload SDK's `crt1.o` and system imports instead of the title converter:

- `--whole-archive libvulkan_radeon.ps5.a`, because Mesa's dispatch tables name
  entry points through weak references and a weak reference pulls no member in;
- `libc++.a`, `libc++abi.a`, `libunwind.a` for ACO and libc++;
- `libps5platform.a` from the SDK fork: the direct-memory heap, thread stacks,
  `open_memstream`, locale, regex, directory functions and the other libc
  functions no system module exports;
- Clang's `libclang_rt.builtins-x86_64.a` for `__emutls_get_address`;
- `--wrap=pthread_{create,join,detach}`, `--wrap=malloc…getdelim`, and about 90
  `--defsym=NAME=ps5_NAME` bindings (the recipe's list);
- `-nodefaultlibs`, then explicit `-lSceLibcInternal -lkernel_web -lSceVideoOut`.
  `libc.a` is deliberately **not** linked: its C11 `thrd_*`/`mtx_*`/`cnd_*`
  collide with RADV's `src/c11/impl/threads_posix.c`, which the whole archive
  already provides.
- `-Wl,--build-id=sha1`, so a clean rebuild is byte-identical (the PS5 link's
  default build-id is a random UUID). The probe rules pass it too, so their
  hashes in 3.3 are stable.

`fill.comp` is compiled at build time with

```sh
glslangValidator -V --target-env vulkan1.1 --vn vk_hello_fill_spv \
    core/render/shaders/fill.comp -o build/gen/fill_spv.h
```

and embedded as a `const uint32_t[]`; no shader file has to reach the console.
`spirv-val --target-env vulkan1.1` accepts the module (`LocalSize 64 1 1`,
descriptor set 0, binding 0).

### 3.2 AGC import stubs

RADV's PS5 winsys imports exactly five AGC entry points: `sceAgcInit`,
`sceAgcSuspendPoint`, `sceAgcDriverSubmitDcb`, `sceAgcDriverSetTFRing`,
`sceAgcDriverSetHsOffchipParam`. The v0.42 payload SDK (the base of the fork)
ships no `libSceAgc`/`libSceAgcDriver` stub, so `platform/ps5/radv` builds the
two import stubs from PS5_Vulkan's canary sources (GPL-3.0-or-later) with
`prospero-lld --shared -soname libSceAgc.sprx`. They are link-time import tables
with the system modules' SONAMEs; the stub bodies are never executed. Whether a
**payload's** loader resolves those imports from the console's modules is the
first console question (see 7.1, `probe_agc.elf`).

`probe_agc.elf` imports all three modules vk_hello names — `libSceAgc.sprx`,
`libSceAgcDriver.sprx`, `libSceVideoOut.sprx` — and prints the address of every
entry point RADV uses (five AGC, three VideoOut: `sceVideoOutOpen`,
`sceVideoOutClose`, `sceVideoOutGetFlipStatus`). It calls only `sceAgcInit` and
`sceAgcSuspendPoint`, which are safe without a device; it submits nothing and
does not open the display, because taking VideoOut's handle from the home
screen is a change the probe has no reason to make. A non-zero address means the
loader bound the import; with BIND_NOW, an unresolved import stops the payload
before `main`, so the start line is the other half of the answer.

### 3.3 Resulting ELF

```
platform/ps5/vk_hello/vk_hello.elf
  101,540,688 bytes (96.8 MiB), SHA-256 e17c2e32fb25a2591dfa50bdd6ef2ebf7b0d5c0aeb28679c05bc82a117a0ec27
  ELF64, DYN, x86-64, entry 0x0, FLAGS_1 PIE, no TEXTREL
  NEEDED: libSceAgc.sprx, libSceAgcDriver.sprx, libSceLibcInternal.sprx,
          libkernel_web.sprx, libSceVideoOut.sprx
  .text ~8 MB, .data ~15.5 MB, .bss ~0.5 MB (debug sections are most of the file size)
  290 strong undefined dynamic symbols, 4 weak; all exported by the NEEDED stubs

platform/ps5/vk_hello/probe_agc.elf
  119,008 bytes (0.1 MiB), SHA-256 6b5cd89b4a796241d4307260bb1b1189dc87d364f9da6295ed58d5136d0d4593
  NEEDED: libSceAgc.sprx, libSceAgcDriver.sprx, libSceVideoOut.sprx,
          libkernel_web.sprx, libSceLibcInternal.sprx, libSceNet.sprx
  12 strong undefined, 0 weak: the 8 AGC/VideoOut entry points plus
  __stdoutp, fflush, puts, vsnprintf (the SDK's default probe link adds libSceNet)
```

`libSceVideoOut.sprx` is required because the whole archive pulls RADV's
VideoOut WSI, which `wsi_device_init` reaches even for a headless device. This
is a runtime dependency of the probe, not an intent to present.

Static verification (no console):

```sh
platform/ps5/vk_hello/verify-elf.sh vk_hello.elf
```

checks DYN/PIE/no-TEXTREL, entry 0x0, no `PT_INTERP`, 0x4000 LOAD alignment,
reads `NEEDED`, and resolves every undefined `.dynsym` entry against the SDK
stubs (plus the AGC stubs in `radv-ps5/lib`). It runs on the probes too. The
full outputs are `$SPLAT_DATA_DIR/runs/F3/verify-vk_hello.txt` and
`verify-probe_agc.txt`, both **PASS**; the pre-fix run's log is
`runs/B1/verify.txt`.

## 4. What vk_hello does

1. Logs the payload start, firmware (`kern.sdk_version`), sets the driver
   environment, and turns RADV's stderr into klog lines through
   `ps5_klog_capture_stderr("[radv]")`.
2. `vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance")`; creates a Vulkan 1.1
   instance with no extensions and no loader.
3. Enumerates physical devices; logs `deviceName`, vendor/device IDs, API and
   driver version.
4. Logs every queue family (count, graphics/compute/transfer/sparse flags,
   timestamp bits) and picks graphics+compute.
5. Logs every memory heap and memory type, then a one-line summary of the
   layout (heap count, type count, device-local GiB). The PS5 winsys derives
   the heap from `sceKernelGetDirectMemorySize()`, so with the unified-heap
   setting (below) the expected heap is one device-local heap of about
   11.44 GiB (the exact figure in a payload process is one of the
   measurements).
6. Creates a device and one graphics+compute queue.
7. Allocates a 1 MiB storage buffer (262,144 uints) in host-visible,
   host-coherent memory (unified heap), fills it with `0xA5A5A5A5`.
8. Creates the embedded `fill.comp` module, descriptor set layout, pipeline
   layout and compute pipeline; logs the pipeline time (includes ACO's compile).
9. Dispatches 4096 workgroups of 64, barriers compute→host, submits with a
   fence and waits up to 10 s.
10. Reads back and verifies `values[i] == i*3 + 7` for all 262,144 elements.
11. Before destroying anything, waits for the submission on its fence for a
    bounded 2 s. Only a signalled fence (`gpu_may_run` cleared) allows the
    reverse-order teardown and `vkQueueWaitIdle`, which then cannot block on
    GPU work. A fence timeout or `VK_ERROR_DEVICE_LOST` — or a `vkQueueSubmit`
    that failed after the batch may have been taken — logs
    `GPU did not finish ... the GPU may still run` and **leaks** the device,
    queue, buffer and memory: nothing is destroyed or freed under a dispatch
    that may still read it. The process exits either way, so the leak lasts
    only as long as the payload does.
12. Logs `PASS`/`FAIL` and exits 0/1. Every line goes to stdout (elfldr's
    socket) and `klog_puts`; a launch through ProsperoMgr leaves only the klog
    copy observable.

Two driver settings are made before the first GPU call, as PS5LM makes them:

- `MESA_SHADER_CACHE_DISABLE=true`: a payload has no title folder for RADV's
  default `/app0/radv-shader-cache`.
- `radv_enable_unified_heap_on_apu=true`: without it RADV splits an APU's pool
  into a 2/3 heap plus a 1/3 heap (~7.6 + ~3.8 GiB); with it the device reports
  the whole direct-memory pool as one device-local heap, which is what the
  11.44 GiB expectation in 7.2 and PS5LM's `setenv` in `ps5/app/main.cpp` both
  assume.

## 5. Files written

| File | What |
| --- | --- |
| `platform/ps5/vk_hello/main.cpp` | the payload |
| `platform/ps5/vk_hello/probe_agc.cpp` | the three-module import probe (AGC, AGC driver, VideoOut) |
| `platform/ps5/vk_hello/Makefile` | SPIR-V embed, RADV link recipe, probes |
| `platform/ps5/vk_hello/verify-elf.sh` | static ELF/import check |
| `platform/ps5/vk_hello/run-on-console.sh` | one command: upload, launch, capture |
| `platform/ps5/vk_hello/.gitignore` | `*.elf`, `build/` |
| `platform/ps5/radv/Dockerfile` | amd64 Debian trixie + LLVM 19 toolchain |
| `platform/ps5/radv/build-in-docker.sh` | host entry point (docker cp + create/start) |
| `platform/ps5/radv/build-in-container.sh` | SDK install, RADV build, AGC stubs, copy out |
| `platform/ps5/radv/README.md` | usage and pinned revisions |
| `docs/research/spike-b.md` | this file |

Outside the repo (not committed): `$SPLAT_DATA_DIR/ref/radv-ps5/` (archive,
headers, SDK, builtins, AGC stubs, `BUILD.txt`) and
`$SPLAT_DATA_DIR/runs/B1/` (`radv-build.log`, `verify.txt`); the rebuild's
static checks are `$SPLAT_DATA_DIR/runs/F3/verify-vk_hello.txt` and
`verify-probe_agc.txt`.

## 6. Risks this build leaves open

- **AGC imports from a payload.** The payload imports `libSceAgc.sprx` and
  `libSceAgcDriver.sprx` by NEEDED. A title gets them through its module set; a
  payload's loader may or may not resolve them. `probe_agc.elf` imports,
  resolves and reports all three modules in six seconds, before the 97 MiB
  payload is deployed.
- **VideoOut import.** RADV's WSI object is in the archive whole and imports
  `libSceVideoOut.sprx`. If the payload process has no VideoOut module, the load
  fails; `probe_agc.elf` tests that import directly (addresses only, no display
  opened). A future, headless-only build could drop the archive's WSI objects,
  but the dispatch tables need the whole archive (see `radv-link.sh`).
- **Heap size in a payload.** 11.44 GiB is the title figure. A payload runs
  beside the home screen; `probe_mem.elf` measures the same pool and the
  `heap[0]` line reports what RADV actually gets.
- **Direct-memory heap collision.** The platform layer wraps `malloc` into a
  reserved direct-memory range and RADV's winsys places buffers in its own
  256 GiB region at `0x4000000000`. If the reservation fails, the platform falls
  back to libc's heap, but RADV buffer allocation could still fail.
- **Device lost on submission.** A known RADV/llama.cpp issue for long compute
  jobs; this one dispatch is tiny, so a timeout here would point at submission
  plumbing rather than job length.

## 7. Tomorrow on the console

The console was in sleep mode all night; nothing was run or installed. The
commands below are the whole loop (`platform/ps5/scripts/run.sh` starts the log
server through ProsperoMgr, uploads the ELF, launches it and captures klog).

### 7.1 Order

```sh
cd "$SPLAT_DATA_DIR/ps5-splat-vr"

# 1. First, ~6 s: can a payload's loader resolve all three system modules the
#    big payload needs (libSceAgc, libSceAgcDriver, libSceVideoOut)? If this
#    fails, vk_hello would fail the same way, at 97 MiB of upload.
PS5_RUN_READY='vkhello-agc' \
    platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_agc.elf 6

# 2. Optional, ~10 s: does a payload see direct memory? (independent of AGC)
PS5_RUN_READY='vkhello-probe' \
    platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_mem.elf 8

# 3. The payload. The wrapper sets a 180 s HTTP timeout for the first 96.8 MiB
#    upload and captures 120 s of log; the same as run.sh with those settings.
platform/ps5/vk_hello/run-on-console.sh 120
```

`run.sh` exit codes: 0 start line seen, 1 no start line, 2 usage, 3 no log
stream, 4 deploy failed, 5 the log stayed empty. The capture is also written to
`$SPLAT_DATA_DIR/runs/console/<UTC-stamp>-vk_hello.log`.

If `run.sh` is not wanted, the two halves work separately:

```sh
platform/ps5/scripts/klog.sh console.log            # terminal 1
platform/ps5/scripts/deploy.sh platform/ps5/vk_hello/vk_hello.elf   # terminal 2
```

### 7.2 Expected log lines

klogsrv prefixes every line with the loader's process name, which is
`payload.elf`, not the uploaded file name.

`probe_agc.elf` (six seconds, eight imports, two calls):

```
<118>[payload.elf] vkhello-agc: start (ps5-splat-vr, linked AGC, AGC driver and VideoOut imports)
<118>[payload.elf] vkhello-agc: import libSceAgc.sprx sceAgcInit @ 0x…
<118>[payload.elf] vkhello-agc: import libSceAgc.sprx sceAgcSuspendPoint @ 0x…
<118>[payload.elf] vkhello-agc: import libSceAgcDriver.sprx sceAgcDriverSubmitDcb @ 0x…
<118>[payload.elf] vkhello-agc: import libSceAgcDriver.sprx sceAgcDriverSetTFRing @ 0x…
<118>[payload.elf] vkhello-agc: import libSceAgcDriver.sprx sceAgcDriverSetHsOffchipParam @ 0x…
<118>[payload.elf] vkhello-agc: import libSceVideoOut.sprx sceVideoOutOpen @ 0x…
<118>[payload.elf] vkhello-agc: import libSceVideoOut.sprx sceVideoOutClose @ 0x…
<118>[payload.elf] vkhello-agc: import libSceVideoOut.sprx sceVideoOutGetFlipStatus @ 0x…
<118>[payload.elf] vkhello-agc: imports resolved 8/8
<118>[payload.elf] vkhello-agc: sceAgcInit(8) -> 0x00000000
<118>[payload.elf] vkhello-agc: sceAgcSuspendPoint() -> 0x00000000
<118>[payload.elf] vkhello-agc: PASS
<118>[payload.elf] vkhello-agc: done
```

All eight addresses are non-zero once the payload starts at all (BIND_NOW: an
unresolved import stops it before `main`). `imports resolved 8/8` and
`sceAgcInit(8) -> 0` are the two lines to read; a non-zero AGC code is the
module's own answer and matches what RADV would get.

`vk_hello.elf`:

```
<118>[payload.elf] vk_hello: start (ps5-splat-vr, RADV linked directly, no loader)
<118>[payload.elf] vk_hello: firmware 13.60 (kern.sdk_version 0x13600007)
<118>[payload.elf] vk_hello: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)
<118>[payload.elf] vk_hello: driver stderr capture on
<118>[payload.elf] vk_hello: instance created (Vulkan 1.1 request, no extensions)
<118>[payload.elf] vk_hello: physical devices: 1
<118>[payload.elf] vk_hello: device: <RADV device name>
<118>[payload.elf] vk_hello: vendor 0x1002 device 0xNNNN type N
<118>[payload.elf] vk_hello: api 1.4.N driver 0x…
<118>[payload.elf] vk_hello: queue families: 1
<118>[payload.elf] vk_hello: family[0] queues 8 flags graphics compute transfer  timestamp bits 64
<118>[payload.elf] vk_hello: using family[0]
<118>[payload.elf] vk_hello: memory: 1 heaps, N types
<118>[payload.elf] vk_hello: heap[0] 11.44 GiB (NNNN bytes) flags 0x1 device-local
<118>[payload.elf] vk_hello: type[N] heap 0 flags 0x… host-visible host-coherent
<118>[payload.elf] vk_hello: heap layout: 1 heap(s), N type(s), device-local 11.44 GiB in 1 heap(s)
<118>[payload.elf] vk_hello: device created
<118>[payload.elf] vk_hello: setup … ms
<118>[payload.elf] vk_hello: buffer 1024.0 KiB, memory type N (host-coherent)
<118>[payload.elf] vk_hello: buffer mapped at 0x…
<118>[payload.elf] vk_hello: compute pipeline created
<118>[payload.elf] vk_hello: pipeline … ms (includes RADV's ACO compile)
<118>[payload.elf] vk_hello: dispatch 262144 elements, 4096 workgroups, submit+wait … ms
<118>[payload.elf] vk_hello: fill verified: 262144/262144 elements == i*3+7
<118>[payload.elf] vk_hello: total … ms
<118>[payload.elf] vk_hello: teardown … ms
<118>[payload.elf] vk_hello: PASS
<118>[payload.elf] vk_hello: done, exit 0
```

`memory: 1 heaps` and `heap layout: 1 heap(s) … in 1 heap(s)` are the new
unified-heap expectation; two heaps (~7.6 + ~3.8 GiB) mean the env var did not
reach RADV. On a **failure** path where the GPU did not finish, the teardown
instead prints

```
<118>[payload.elf] vk_hello: FAIL vkWaitForFences -> VK_TIMEOUT (10 s)
<118>[payload.elf] vk_hello: teardown: GPU did not finish in 2000 ms -> VK_TIMEOUT; the GPU may still run
<118>[payload.elf] vk_hello: teardown: leaking device, queue, buffer and memory (no destroy or free)
<118>[payload.elf] vk_hello: teardown … ms
```

The last line is short: nothing is visited, so the leak costs no time.

The heap byte count is whatever `sceKernelGetDirectMemorySize()` reports; the
11.44 GiB figure is the title measurement, not a hard-coded check. The device
name comes from RADV and is not asserted. `run.sh` matches the first line
against the ELF stem `vk_hello`, case-insensitively; the probes' start lines do
not match their stems (`probe_agc.elf` prints `vkhello-agc: start`), which is
why 7.1 sets `PS5_RUN_READY` for them.

RADV's own messages, when it fails, appear as:

```
<118>[payload.elf] [radv] radv/ps5: sceAgcInit(…) failed: 0x…
<118>[payload.elf] [radv] radv/ps5: sceAgcDriverSubmitDcb failed: 0x…
```

### 7.3 What each failure means

| Symptom | Meaning | Next step |
| --- | --- | --- |
| `run.sh` exit 3 | no klog stream: ProsperoMgr or klogsrv unavailable | open `http://$PS5_HOST:7070`, check `/api/elfldr/status`; `klog.sh --ensure` |
| exit 4 | deploy failed: ProsperoMgr refused or elfldr on its loopback is down | check the deploy lines; ProsperoMgr `reachable:true` |
| exit 5, zero bytes | klog connected but no line at all; the payload may have been killed before its first write | retry with `klog.sh` connected first; check `/api/processes` for `payload.elf` |
| exit 1, no start line | the loader rejected the ELF or it died before `main` (missing NEEDED module) | read `probe_agc.elf`'s result; if its start line is missing too, the AGC/VideoOut imports are the cause; also try a smaller payload (`hello.elf`) to prove the loop |
| `probe_agc` start line, `imports resolved N/8` or no `PASS` | a module did not map for a payload process (which one is visible in the address lines) | report the module; the big payload will fail the same way — do not deploy it |
| `probe_agc`: `sceAgcInit(8) -> 0x…` non-zero | the loader resolves AGC but the module refuses init in a payload process | report the code; RADV would get the same one |
| start line, then nothing | crash between `main` and the first klog write (unlikely; heap/thread init) | look for `[elfldr.elf]`, `[payload.elf]` or process-exit lines |
| `driver stderr capture off` | the platform could not make the stderr pipe/thread; RADV errors are lost | non-fatal for the probe; note it |
| `FAIL vk_icdGetInstanceProcAddr has no vkCreateInstance` | the archive is not linked whole or the wrong SDK | rebuild (`rm platform/ps5/vk_hello/vk_hello.elf`, then make); rerun `verify-elf.sh` |
| `FAIL vkCreateInstance -> …` | AGC init or RADV instance creation failed; look for `[radv]` lines above | `probe_agc.elf` isolates the AGC import; `MESA_DEBUG`/`RADV_DEBUG` are not set |
| `physical devices: 0` | RADV enumerated no device (winsys init failed) | same as above; AGC or direct memory |
| `FAIL no compute queue family` | unexpected device shape; report the family lines | check RADV build options |
| `heap[0]` far below 11.44 GiB | direct memory is smaller in a payload process than in a title; not fatal for this probe | `probe_mem.elf` gives the same pool; record the number |
| two heaps, ~7.6 + ~3.8 GiB | `radv_enable_unified_heap_on_apu` did not reach RADV (env set after the GPU opened, or the driver ignored it) | the payload sets it before the instance; check the line above `driver stderr capture`; record both heap sizes |
| `FAIL no host-visible memory type` / `FAIL vkAllocateMemory` / `vkMapMemory` | RADV did not expose host-visible unified memory to this process | this is a real gate for the renderer; record and report |
| `FAIL vkCreateComputePipelines` | ACO failed to compile `fill.comp`; the klog `[radv]` lines say why | set `RADV_DEBUG=shaders` in `main.cpp`, rebuild, and rerun |
| `FAIL vkQueueSubmit` / `vkWaitForFences` (10 s) | AGC submission failed or the GPU never signalled; device lost | the most serious result: report the AGC return code from `[radv]`; teardown then logs `leaking …` and frees nothing, which is correct |
| `teardown: leaked` / `GPU did not finish` | teardown saw the submission unfinished and refused to free the buffer or destroy the device; the process exits with the leak | expected after the row above; not a new fault. Note whether `vkQueueSubmit` returned first |
| `FAIL fill mismatch` | the GPU ran but the buffer contents are wrong; a driver or descriptor bug | record index/values; try a `vkCmdFillBuffer` variant to separate dispatch from memory |
| `PASS` but `total` very large | shader cache disabled and/or ACO cold compile; measure on a second run | record both times |

### 7.4 What lands on the console

Tomorrow's run first uploads `probe_agc.elf` (119,008 bytes) and, if wanted,
`probe_mem.elf`, then `vk_hello.elf` (101,540,688 bytes), each to
`/data/ProsperoMgr/payloads/<name>.elf`; each later deploy overwrites it.
Nothing is deleted, renamed, disabled, installed as a PKG, or changed in
settings or firmware. Add the row to `platform/ps5/README.md` after the run,
as the project's console record requires; that file is outside this task's
allowed area, so it is untouched here.

## 8. Open questions for the next console session

- Does a payload's loader resolve `libSceAgc.sprx`, `libSceAgcDriver.sprx` and
  `libSceVideoOut.sprx` from the console's loaded modules? (`probe_agc.elf`
  imports, resolves and reports all three; then vk_hello's start line.)
- Is the RADV heap in a payload the title's 11.44 GiB, or less?
- Does `sceAgcDriverSubmitDcb` return 0 for a compute-only submission from a
  payload, and how long does the first ACO compile take there?
- Can the platform heap's 16 GiB range be reserved beside the home screen?

## Sources

- [PS5LM `docs/BUILDING.md`](https://github.com/cobanov/PS5LM/blob/main/docs/BUILDING.md)
  and `.github/workflows/native-build.yml` (`debian:trixie-20261020-slim`,
  LLVM 19, `BUILD_JOBS=2`)
- [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) `tools/build-radv.sh`,
  `tools/radv-link.sh`, `docs/RADV_PHASE.md`, `radv/radv_smoke.c`
- [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) `src/amd/vulkan/winsys/ps5/`
- [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) `platform/`
  (`libps5platform.a`, `heap.c`, `threads.c`, `klog.c`)
- Console, log and deploy details: `docs/research/ps5-console.md` sections 6–8
