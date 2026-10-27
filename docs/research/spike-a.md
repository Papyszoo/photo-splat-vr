# Spike A, step 1: ggml-vulkan + reconstruction payloads for the console

Research and build record for Task A5 and the pre-run review fixes (F6),
2026-10-26. Host: macOS 26.5, arm64 (Apple M4, 16 GiB). The console **was off
for the whole task**: nothing was sent to it, nothing was installed,
`push-data.sh` was not run against it (it was exercised against a local stand-in
for ProsperoMgr's file routes, section 6.3). The deliverable is three bench
payloads plus the one homebrew title that runs all three with a direct-memory
pool (§2.4), the cross-build glue, the transfer script and this runbook.

## 1. Route: cross-compile on the Mac, link RADV whole, no loader

The same route `vk_hello` proved (docs/research/spike-b.md §3.1), extended to a
library stack:

```sh
platform/ps5/ggml/build.sh                  # ggml + core -> build-ps5-ggml/*.a
make -C platform/ps5/bench                  # the three payloads
make -C platform/ps5/bench bench-title      # the three benches as ONE homebrew title
platform/ps5/bench/verify-elfs.sh           # static checks of the payloads, no console
```

The title package is `build/ps5/title/PPSA99201/` (68.7 MiB): the same three
benches linked into one eboot whose program reads the bench to run from
`/data/ps5-splat-vr/bench/bench.env` (§2.4). It is the route the console run
uses, because a payload has 0 B of direct memory and RADV therefore gives it no
heaps (`docs/research/spike-b-memory.md` §1-2); the payloads stay buildable as
the control. `platform/ps5/bench/run-bench.sh --title <bench>` drives the title
route end to end (§7).

| Item | Value |
| --- | --- |
| Toolchain | payload SDK `prospero-clang` 21.1.8, target `x86_64-sie-ps5`, sysroot `$SPLAT_DATA_DIR/ref/radv-ps5/sdk` |
| SDK revision | `3e31e0b1b68ada28beca68adc480cf0c3d17b9df` (the RADV build's own tree, BUILD.txt) |
| ggml | submodule `a05c9922`, v0.27.0, CPU + Vulkan backends |
| Shaders | 155 `.comp`, compiled **on this host** by ggml's `vulkan-shaders-gen` (glslc/shaderc v2026.4), embedded as `uint32_t[]` |
| Vulkan headers (compile) | host Vulkan-Headers 1.4.363 (Vulkan-Hpp asserts the C/C++ header pair match; RADV's 1.4.354 `include/` has no `vulkan.hpp`) |
| Vulkan at link | `libvulkan_radeon.ps5.a` whole archive, the same archive and recipe as `vk_hello` |
| Clean build | **62.6 s wall**, 307 s CPU, 8 jobs, 194 build steps (155 of them shaders) |
| Link inputs | payload SDK `libc++/libc++abi/libunwind/libps5platform` + Clang builtins, `--wrap=malloc…`, ~90 `--defsym=NAME=ps5_NAME` |
| AGC | RADV's `libSceAgc.so`/`libSceAgcDriver.so` import stubs + `libSceVideoOut`, as `vk_hello` |

Libraries produced (`build-ps5-ggml/`):

| Archive | Size |
| --- | ---: |
| `libggml-base.a` | 5.2 MB |
| `libggml-cpu.a` | 4.9 MB |
| `libggml.a` | 0.46 MB |
| `libggml-vulkan.a` | 54.8 MB |
| `libsplat_core_ps5.a` | 8.2 MB |
| `libsplat_miniz.a` | 0.39 MB |
| `libsplat_ps5_glue.a` | 0.75 MB |

Details of the glue — the stand-in `Vulkan` package, the host shader generator,
and the four Vulkan symbols + three libc symbols a payload must supply — are in
`platform/ps5/ggml/README.md`. **No ggml source is patched**; `patches/` gained
nothing for this task.

## 2. The three benches: one payload each, and one title for all three

All three: the two RADV settings `vk_hello` makes before its first GPU call
(`MESA_SHADER_CACHE_DISABLE=true`, `radv_enable_unified_heap_on_apu=true`),
bounded waits via a watchdog (section 5), and "log and exit 2" on a missing
file. The two reconstruction benches submit through the core's `GraphSession`
(a small graph per checkpoint); `op_audit`'s cases are single-op graphs of 1-8
nodes, below `GraphSession`'s 64-node submit budget, so each is already one
submit and there is nothing to split (section 2.1). A compute error or a
watchdog timeout logs and abandons the process **without tearing anything
down**; only a clean read-back frees a buffer (section 5). Each logs to stdout
(elfldr) **and** `klog_puts`, so a launch through ProsperoMgr still shows every
line in the klog stream. Both RADV settings are applied without overwriting, so
`bench.env` can still change them.

| ELF | Full | Deployable (`--strip-debug`) | What it measures |
| --- | ---: | ---: | --- |
| `op_audit.elf` | 150.5 MiB | 67.4 MiB | 35 ggml cases at SHARP/DA2 shapes, Vulkan vs the console CPU, twice (coopmat on/off) |
| `da2_bench.elf` | 153.1 MiB | 67.8 MiB | C4 fallback end to end on a photo: decode → ViT-S/DPT → `.psplat` |
| `sharp_bench.elf` | 153.9 MiB | 67.9 MiB | C3 SHARP stages through the C2 interface → `.psplat` (1.18 M splats) |

Static checks (`platform/ps5/bench/verify-elfs.sh`, which reuses
`platform/ps5/vk_hello/verify-elf.sh`): **PASS** on all three — DYN + FLAGS_1
PIE + no TEXTREL, entry 0x0, no `PT_INTERP`, every `LOAD` at 0x4000, and every
undefined dynamic symbol resolved by the NEEDED stubs.

```
NEEDED (all three): libSceAgc.sprx, libSceAgcDriver.sprx, libSceLibcInternal.sprx,
                    libkernel_web.sprx, libSceVideoOut.sprx
undefined:          311 (op_audit) / 313 (da2_bench) / 316 (sharp_bench) strong, 4 weak
```

The NEEDED set is exactly `vk_hello`'s, so the AGC/VideoOut question Spike B
answers first applies unchanged (run `probe_agc.elf` before the first big
upload). Logs: `$SPLAT_DATA_DIR/runs/A5/verify-<name>.txt`, `verify-all.txt`.

The same sources are also the program of **one** homebrew title (§2.4), which
is what the console run uses: a payload has 0 B of direct memory, so RADV gives
it no heaps and every GPU allocation in these benches fails. The payloads are
kept as the negative control (they show `memory: 0 heaps`, exactly as
`vk_hello.elf` does), and `run-bench.sh --payload` still runs them.

### 2.1 op_audit — the console RADV re-run the op audit needs

`tools/op-audit/sharp_ops.cpp` ported to a payload: the same cases and
statistics, plus the DA2 shapes, one process, klog output and a `bench.env` file
that selects driver settings before the first Vulkan call. 35 cases (a maximum
of 8 graph nodes each, so one submit per case -- see the split note below):

* SHARP: patch-embed conv (direct, F16 weights, `ggml_conv_2d` F16 im2col, F32
  im2col), 3x3 convs at 96²/768², 1x1 fuse_lowres, 2x2 s2 skip conv, TConvs
  (48→96, 192→384, 768→1536), LayerNorm 1024 over 35×577, GELU-erf 4096×20195,
  qkv/proj mul_mat F32+F16, flash-attn 577 tokens ×16 heads ×35 patches KV
  F16+F32, GroupNorm 8g at 96²/768², 3x3 128ch head, max/avg pool at 1536²,
  broadcast add, the composer activation chain, interpolations (1536→768,
  1280×873→1536 align_corners).
* DA2: stem 14×14 s14 3→384 at 518², the same as F32 im2col, qkv 384×1152
  n=1370 F16, attention 1370 tokens ×6 heads F16 KV, 3x3 64ch at 148²,
  TConv k4s4 37→148, interpolation 37→148 align_corners.

**No GraphSession.** The core's `GraphSession` splits a long graph into 64-node
submits. Every case here is one op or a short chain (im2col + reshape +
mul_mat + permute + cont is the longest at 8 nodes), so one
`ggml_backend_graph_compute` per case is already the benches' splitting policy
applied to a graph that cannot be split further. The payload logs
`max 8 nodes per graph, 1 submit each (below the 64-node GraphSession budget)`
so the claim is visible on the console, and a future case that outgrows the
budget is flagged `ABOVE 64: a split is needed`.

**Coopmat on and off in one launch.** Cooperative matrices are chosen when
ggml-vulkan builds its one cached `VkDevice`, so a second backend in the same
process would reuse the same device and the same setting. After pass 1 the
payload frees its buffers and backend, releases the cached device through
`platform/ps5/ggml/vk_reset.cpp` (the same `vk_instance` definition from the
ggml tree, no ggml patch), flips `GGML_VK_DISABLE_COOPMAT` and runs every case
again. The two results are logged side by side:

```
op_audit: ---- coopmat on/off, side by side (pass 1 on, pass 2 off) ----
op_audit: sharp.stem_16x16s16_3to1024_384             PASS     92.1 ms nmse 6.7e-07  | PASS    105.6 ms nmse 2.7e-13
...
```

If the driver exposes no cooperative matrix (pass 1 `off` with no env override),
both settings are the same path and the second pass is skipped with a log line.
A forced `GGML_VK_DISABLE_COOPMAT=1` in `bench.env` simply makes pass 1 the
disabled one and pass 2 the enabled one; no manual edit is needed for the
comparison.

Gates per case: `exact` (F32 weights) NMSE ≤ 1e-6 and cos ≥ 0.99999;
`reduction` (F32 flash attention, 577-term reduction) NMSE ≤ 1e-5 and cos ≥
0.9999; `f16` (F16 weights or F16 staging) NMSE ≤ 1e-4 and cos ≥ 0.999;
`finite` (no CPU reference, 1.2 GB tensors) finite output only. A case that the
device rejects prints `UNSUP` with the op and shape. The exit code is 0 only if
both passes pass.

The decisive line is printed per pass, comparing the two patch-embed paths:

```
op_audit: stem parity coopmat=on: F32 im2col nmse 6.82e-08, ggml_conv_2d nmse 1.73e-05 (253.2x worse) -> use F32 im2col
```

### 2.2 da2_bench — C4 on a photo from the console

`/data/ps5-splat-vr/bench/photo.jpg` (or the first `*.jpg` there, or argv[1]) and
`/data/ps5-splat-vr/models/dav2s-f16.gguf` (falls back to the f32 file). Decodes
with the project's stb wrapper, takes the EXIF focal, runs `FallbackReconstructor`,
writes `<photo stem>.psplat` next to the photo, and logs per-stage time, the
submit count, the heap before/after load and run, and peak RSS.

### 2.3 sharp_bench — C3 through the C2 interface

Same input convention, with `sharp_2572gikvuh-f16-v1.gguf` (falls back to f32).
It drives `SharpReconstructor`, **not** a fixed stage list, so a newer tree that
adds or reworks a stage runs without touching this payload; the summary prints
every stage's time and leaves a stage at 0.00 s when it is not in the tree.
Writes `<photo stem>.sharp.psplat` (so it does not collide with da2_bench's).

### 2.4 The title: one package, bench selected by `bench.env`

`make -C platform/ps5/bench bench-title` packages the three bench sources as
**one** title, `PPSA99201` (id in `platform/ps5/title/param-bench.json`):

| File | Bytes |
| --- | ---: |
| `eboot.bin` (signed fSELF, 12 segments, integrity valid) | 70,738,967 |
| `sce_module/libc.prx` (hash-checked, same file vk_hello uses) | 1,284,674 |
| `sce_sys/param.json` | 1,052 |
| `sce_sys/icon0.png` | 3,911 |
| **whole package** (what an install uploads) | **72,028,604** (68.69 MiB) |

Its program (`platform/ps5/bench/bench_title.cpp`) reads the first `BENCH=`
from

```
/data/ps5-splat-vr/bench/bench.env      # BENCH=op_audit|da2_bench|sharp_bench
```

and calls `op_audit_run`, `da2_bench_run` or `sharp_bench_run`. Each bench's
common title prologue (`apply_bench_radv_defaults`) logs the direct and flexible
pools first, tagged with that bench, before its first GPU call. The three bench
`.cpp` files expose those entry points and keep their own `main()` only for the
payload build (`#ifndef PS5_TITLE`), so a payload and the title share one
source, not two copies.

Why one title with a selector, not three packages (or three title ids):

* **The launch route has no arguments.** ProsperoMgr's `POST /api/titles/launch`
  ends in `sceSystemServiceLaunchApp(title_id, nullptr, &ctx)`
  (`ref/ProsperoMgr/.../titles.cpp`), so a bench title cannot be told which
  bench to run on the command line. A file next to the data is the only input,
  and `bench.env` already exists there for driver settings. `run-bench.sh`
  rewrites its ~74-byte body before each run (`title.sh put`); the package does
  not change.
* **One install, one tile, one eboot.** The three benches are 99% the same
  bytes (RADV whole + ggml-vulkan + core); three packages would be three ~68 MiB
  eboots, three ShadowMountPlus registrations and three `title.sh install`
  runs. One package is 68.7 MiB, installed once and reused by every bench.
* **The gate title stays intact.** `PPSA99200` is `vk_hello`'s package, the
  gate that proves the title route on the console. Sharing that id would make
  each bench install overwrite the gate's `eboot.bin`. `PPSA99201` is the next
  free provisional id in the same block (`param-bench.json`, content id
  `IV0000-PPSA99201_00-PS5SPLATVRBENCH1`).
* **The cost is one line of state.** A second title id is the price; the
  alternative (one id, rebuild vk_hello ↔ benches) costs two rebuilds and two
  installs per session and loses the gate in between.

A title has no `argv` of its own (the app's process parameters are not ours),
so the dispatcher calls each bench with `(0, nullptr)`: the photo and GGUF come
from the bench directory and `bench.env`, exactly as before. What changes for a
bench in title mode (`bench_title.h`):

| Payload | Title |
| --- | --- |
| `log_line` → stdout + `klog_puts` | → `sceKernelDebugOutText` (same text) |
| `apply_radv_defaults`: cache off + unified heap | cache left at the title default, unified heap on |
| `ggml_logging.h` (`klog_puts`) | `install_title_ggml_logging` (same text) |
| `_exit(n)` on a GPU failure | `abandon()` logs, then `LoadExec("exit")`, no teardown |
| returns from `main` | `sceSystemServiceLoadExec("exit", nullptr)` |
| no direct pool | pool logged before the first Vulkan call |

The bounded-wait policy is unchanged (§5): the same `Watchdog`, the same
deadlines, the same deliberate leak. One caveat the console has to settle: the
watchdog lives in `platform/ps5/ggml/bench_env.cpp` (shared glue, outside this
task's area) and ends the process with `_exit(3)`, which a title may not be
allowed to call (`vk_hello`'s title comment: a title may not `exit()`,
SIGSYS). The two `WATCHDOG` lines are written *before* `_exit(3)`, so a refused
exit still leaves the evidence in klog; the clean and the error paths in the
bench sources go through `abandon()`/`LoadExec` instead. The fix, if the
console shows the trap, is a title-end hook in that shared glue (or a
`GGML_VK_FENCE_TIMEOUT_MS`, §5).

The title also polls `/data/ps5-splat-vr/title-quit` (the file
`title.sh stop` writes) on a background thread: a request logs
`quit requested` and ends the app through `LoadExec` without teardown, so a
bench that is still running can be stopped gracefully. After the bench's
`done, exit` line the app has already ended itself; `title.sh stop` then just
reports "not running".

It writes only under `/data/ps5-splat-vr/` (the `.psplat` outputs and the
selector); RADV's own shader cache stays at the title default the winsys
chooses (`/app0/radv-shader-cache`), as in `vk_hello`'s title build.

## 3. Host smoke run (this Mac, MoltenVK — not the console)

The three sources were also built for the host against the desktop `ggml`
dylibs (macOS `ld64` has no `--wrap`, so `-DSPLAT_BENCH_NO_WRAP` reports 0
submits, and the hidden `vk_instance` needs `-DSPLAT_BENCH_NO_VK_RESET`) and run
against the real weights. This validates the payload logic end to end; it is
**not** a console number.

`op_audit`: **35/35 pass, 0 unsupported, 13.9 s, 5.7 GiB peak RSS** in the
single-pass host smoke. The host `ggml-vulkan` dylib hides `vk_instance`, so
the host binary is built with `SPLAT_BENCH_NO_VK_RESET` and logs
`coopmat=unknown`; the coopmat comparison runs in the console build, where the
same code is linked statically. Selected rows (medians, the host GPU vs the
Mac's CPU):

| Case | ms vk | NMSE | cos |
| --- | ---: | ---: | ---: |
| stem 16×16 s16 3→1024 @384²×35, direct | 105.6 | 2.7e-13 | 1.0000000 |
| same, F16 weights | 92.1 | 6.7e-07 | 0.9999997 |
| `ggml_conv_2d` (F16 im2col) | 14.1 | **1.73e-05** | 0.9999914 |
| F32 im2col + mul_mat | 28.2 | **6.82e-08** | 1.0000000 |
| conv 3×3 1024ch @96² | 565 | 3.2e-12 | 1.0000000 |
| conv 3×3 256ch @768² | 2073 | (finite only) | — |
| TConv 2×2 128ch 768²→1536² | 909 | (finite only) | — |
| qkv 1024×3072 n=20195 F32 / F16 | 93.8 / 44.7 | 6.8e-08 / 2.3e-05 | 1.0000000 / 0.9999885 |
| flash-attn 577×16×35 KV F16 / F32 | 75.3 / 137.4 | 1.26e-06 / 1.29e-06 | 0.9999994 / 0.9999994 |
| DA2 stem 14×14 s14 @518² | 65.8 | 2.0e-13 | 1.0000000 |

Rows that reproduce `docs/research/op-audit.md` exactly (conv 256@768² 2016 ms,
F16 im2col 1.7e-5, F32 im2col 6.8e-8) mean the port is faithful, and the two
paths a RADV re-run has to answer — cooperative-matrix conv and F16 accumulation
— are still open until it runs on the console (section 4). The host run's own
splitting line is `35 cases, max 8 nodes per graph, 1 submit each`.

`da2_bench`: decoded 1280×873 in 0.010 s, focal 1074.3 px from EXIF, model load
0.07 s, **inference 0.55 s**, 499,896 splats, `.psplat` 13.35 MiB, peak RSS
155 MiB, heap 11.79 GiB free of 16.00 GiB.

`sharp_bench`: model load 1.47 s (1.31 GiB), **inference 49.62 s** — pyramid
1.74, patch encoder 15.49, image encoder+upsamplers 0.02, depth decoder 16.08,
initialiser 0.13, Gaussian decoder 13.65, composer 2.50 s — 1,179,648 splats,
`.psplat` 31.50 MiB, **peak RSS 2.49 GiB**, exit 0. That matches
`core/recon/sharp/README.md`'s 49.8-55.1 s / 2.48 GiB, so the payload adds no
overhead to the library.

The 2026-10-26 smoke ran the photo from a directory whose name contains a space
(`photo one.jpg`), which exercises the shared `bench_common.h` path helpers end
to end; the `.psplat` files were written next to it with the expected names.

## 4. What the console still has to answer

| Question | Where it shows |
| --- | --- |
| Does a title get the app pool (not the payload's 0 B)? | `direct pool …` on every bench, before the first GPU call |
| Do AGC / AGC driver / VideoOut bind in a payload? | `probe_agc.elf` first (Spike B's six-second check); if it fails, these payloads fail the same way at 67 MiB each |
| Is the heap one 11.44 GiB pool in a payload? | `heap before load: … free of …` on every run; two heaps or ~7.6 GiB means the unified-heap setting did not reach RADV |
| Does RADV expose `VK_KHR_cooperative_matrix`, and does conv take the f16-accumulator path? | ggml's device line `matrix cores: coopmat1/coopmat2/none`, then `op_audit`'s own `coopmat on/off` side-by-side block (both settings in one launch) |
| Do long submits survive (device-lost risk)? | submit counts and per-stage times; the watchdog line if a wait never returns |
| Is SHARP inside 60 s on the device? | `sharp_bench`'s `timings … total` line |

## 5. Bounded waits and deliberate leaks, without patching ggml

ggml-vulkan waits on its fences with `UINT64_MAX` (5 call sites in
`ggml-vulkan.cpp`), so a lost device or a wedged submission hangs the payload
with no log line — exactly the failure this run is looking for. The payloads
bound it from outside: `Watchdog` (platform/ps5/ggml/bench_env.h) is a thread
that, when a stage's deadline passes, logs the stage and `_exit(3)` **without
tearing anything down**. `op_audit` arms it around both the
`ggml_backend_graph_compute` call and each `ggml_backend_synchronize`, so the
unbounded synchronize is bounded by the same watchdog. da2/sharp arm it around
model load, inference and the scene write. This is the same "leak, do not free
under a GPU that may still run" policy `vk_hello` uses (docs/research/spike-b.md
§4).

The same policy covers a *returned* error, not just a hang:

* `op_audit`: a graph compute that returns `GGML_STATUS_FAILED` (device lost) or
  any other non-success logs the case and `_exit(1)`s directly; the buffer,
  context and device are never freed.
* `da2_bench` / `sharp_bench`: a failed model load or a failed
  `reconstruct( ... )` logs and `_exit(1)`s; the reconstructor's destructor
  (which would free every GPU weight and buffer) never runs.
* Only a successful compute whose output has been read back is freed: that is
  the only point at which the GPU is known to be idle.

Deadlines: op_audit 300 s per case (900 s for the big ones), da2 600 s
inference, sharp 900 s load / 1800 s inference, 120-180 s for a scene write.

A proper fix (a `GGML_VK_FENCE_TIMEOUT_MS` in the five waits, failing the graph
instead of hanging) is a small `patches/ggml/` entry, deliberately deferred:
it would mean editing the ggml submodule other builds are using.
Logged in `docs/BACKLOG.md`.

Submissions are counted without a core API change: the payload link carries
`-Wl,--wrap=ggml_backend_graph_compute`, and `bench_env.cpp` counts every call
(one call = one submit: a `GraphSession` submit in the reconstruction benches,
the single case graph in `op_audit`).

In title mode the policy is the same but the process end differs (§2.4): the
bench sources end a failed load/inference through `abandon()`, which logs and
calls `sceSystemServiceLoadExec("exit", nullptr)` — a title may not `exit()` or
return to the CRT — and the dispatcher ends a clean or bad-selector run the same
way, without running any destructor. The `Watchdog` in the shared glue still
calls `_exit(3)`; its two lines are out before the call, so the evidence
survives even if the title refuses that syscall.

## 6. Getting 1.3 GiB to the console: `push-data.sh`

### 6.1 The four routes that exist

| Route | What it is | Verdict |
| --- | --- | --- |
| **ProsperoMgr native file API** (`/api/ftp/upload/chunk`) | ProsperoMgr already runs for `deploy.sh`/`klog.sh`. `POST /api/ftp/upload/chunk?path=&upload_id=&offset=&total=` appends each chunk into a checkpoint file next to the destination and renames it when complete | **chosen**: no extra payload, resumable, per-chunk retry, never exposes a partial GGUF |
| ftpsrv on 2121 | `$SPLAT_DATA_DIR/ref/ftpsrv` (`make -f Makefile.ps5`), launched through ProsperoMgr, then `curl -T … ftp://host:2121/data/…` | fallback only: needs a build + payload slot + a second service, and FTP gives no resumable checkpoint |
| Garlic SaveMgr (web UI on 8082) | save-file decrypt/browse with drag-and-drop | not usable: it works inside save containers, not arbitrary `/data` paths |
| ProsperoMgr payload upload | `POST /api/payloads/upload` → `/data/ProsperoMgr/payloads/`, `.elf/.bin/.self` only | not usable for data |

ProsperoMgr's chunk route has a 264 MiB body limit and the frontend uses
64-128 MiB chunks; `push-data.sh` uses **64 MiB** (`PUSH_CHUNK_MB`), retries a
chunk 3×, and on a checkpoint mismatch (409) re-reads the checkpoint size and
resumes from it (offset 0 only for a stale/oversized checkpoint). It skips a
file whose remote size already matches and size-verifies every completed
destination, so a re-run never rewrites the 1.22 GiB payload.

### 6.2 What it sends

| Local | Remote | Size |
| --- | --- | ---: |
| `$SPLAT_DATA_DIR/weights/sharp_2572gikvuh-f16-v1.gguf` | `/data/ps5-splat-vr/models/` | 1.22 GiB |
| `$SPLAT_DATA_DIR/weights/dav2s-f16.gguf` | `/data/ps5-splat-vr/models/` | 47.4 MiB |
| `$SPLAT_DATA_DIR/golden/inputs/photo01_living_room.jpg` → `photo.jpg` | `/data/ps5-splat-vr/bench/` | 532 KiB |
| `bench.env` (written if absent) | `/data/ps5-splat-vr/bench/` | <1 KiB |

`PUSH_PHOTO=…` swaps the photo; `PUSH_REMOTE_ROOT` the root (only
`/data/ps5-splat-vr` and below are accepted). Modes: `--check` (plan only, no
console I/O), `--dry-run` (prints every HTTP request it would make, no console
I/O), default push, `--verify` (size compare only). Exit 0 all present, 1 an
upload failed, 2 local file missing / unsafe path, 3 no ProsperoMgr/route.

Safety rules enforced in the script, not just documented:

* every remote path is validated before a request is built: it must be
  `/data/ps5-splat-vr` or a descendant, with no empty, `.` or `..` component,
  no control characters and no trailing slash (one is stripped from the root);
* no delete call exists; the only way a destination changes is ProsperoMgr's
  own rename of a *complete* checkpoint over it;
* the upload id is derived from `path|size`, so an interrupted transfer leaves
  `<name>.pmgr-part-<id>` and the next run resumes at exactly that byte count
  (the size comes from `/api/ftp/list`); a stale/oversized checkpoint restarts
  at 0, which is the one operation that truncates;
* after the server reports `complete`, the script re-lists the destination and
  compares its size with the local file before calling it done;
* the only service contacted is ProsperoMgr on `PS5_PROSPERO_PORT`, over HTTP:
  no FTP, no elfldr, no klog port. `PS5_PROSPERO_PORT` is validated as numeric
  and exported so every request in the script uses the same endpoint;
* local and remote paths are quoted end to end (the plan is three parallel
  arrays, not a `|`-joined string), so spaces and other metacharacters survive.

### 6.3 How it was tested without the console

`push-data.sh` was run against a local stand-in for the four routes it uses
(`/api/status`, `/api/ftp/list`, `/api/ftp/mkdir`, `/api/ftp/upload/chunk`,
same semantics: append-to-checkpoint, `next_offset`, `409` on a size mismatch,
rename at completion) on a loopback port, with 5 MiB / 300 KiB / 532 KiB files
and 1 MiB chunks. All local paths contained a space (`input dir/sharp
model.gguf`, `photo one.jpg`), and the photo's `.psplat` smoke output also had a
space in its name.

Result: the three files and `bench.env` pushed, every destination byte-compared
against its source, `--verify` PASS, second run skipped everything by size.
`--check` and `--dry-run` print the plan/requests and touch the network zero
times. Unsafe roots (`/etc`, `..`, `//`, `./`) exit 2 before any request. A
resume test pre-created the server-side checkpoint at 2 of 5 MiB, and the run
logged `resuming at 2097152 of 5242880 bytes`, sent only the remaining 3 MiB,
and produced a file identical to the source. The stand-in was a scratch file
and is not in the repo.

Upload time on the LAN is not known until it runs; at a conservative 10 MB/s the
1.27 GiB is ~2.2 min, at 1 MB/s ~22 min (the script reports MiB/s every 5 s).

## 7. Tomorrow on the console

Console awake, ProsperoMgr on 7070, elfldr reachable (ProsperoMgr's
`/api/elfldr/status`), klogsrv starting on 3232. Nothing is deleted; the only
files written are under `/data/ps5-splat-vr/` (the data files, the selector and
the `.psplat` outputs), the payloads, which land in
`/data/ProsperoMgr/payloads/` with their own names, and the bench title package
under `/data/homebrew/PPSA99201/`.

```sh
cd "$SPLAT_DATA_DIR/ps5-splat-vr"     # the repo; .env.local has PS5_HOST

# 0. Six seconds: do AGC, AGC driver and VideoOut bind in a payload?
PS5_RUN_READY='vkhello-agc' \
    platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_agc.elf 6

# 1. Data: 1.27 GiB over ProsperoMgr's chunked route (~2 min at 10 MB/s).
platform/ps5/scripts/push-data.sh --check      # optional: what will be sent
platform/ps5/scripts/push-data.sh --dry-run    # optional: every HTTP request
platform/ps5/scripts/push-data.sh
platform/ps5/scripts/push-data.sh --verify

# 2. The bench title: ONE install, then one bench per run (see 2.4).
platform/ps5/bench/run-bench.sh --title --dry-run op_audit   # plan, no console I/O
platform/ps5/bench/run-bench.sh --title op_audit             # 1200 s window
platform/ps5/bench/run-bench.sh --title da2_bench            # 300 s
platform/ps5/bench/run-bench.sh --title sharp_bench          # 900 s
```

`run-bench.sh --title` writes `BENCH=<name>` to
`/data/ps5-splat-vr/bench/bench.env` (`title.sh put`), installs
`build/ps5/title/PPSA99201` if `title.sh status` does not already verify it,
launches it, streams klog to `$SPLAT_DATA_DIR/runs/console/<UTC>-<name>-title.log`
until that bench's `done, exit` line, and stops the title (`title.sh stop`;
after a normal end the app has already ended itself). The title id and the two
data files are the only things that change between benches; the 68.7 MiB package
is installed once. `--env FILE` adds driver settings to the generated
`bench.env` (a `BENCH=` line in the file is replaced).

`op_audit` needs no `bench.env` edit and no second launch: pass 1 runs at the
driver default (or the `bench.env` value if one is set), then the payload
releases the cached `VkDevice`, flips `GGML_VK_DISABLE_COOPMAT` and runs pass 2.
The capture window is 1200 s because both passes together are roughly twice the
one-pass time on the console CPU reference. If RADV exposes no cooperative
matrix, pass 1 logs `coopmat=off` and the payload skips pass 2 with a reason;
if a forced `GGML_VK_DISABLE_COOPMAT=1` is in `bench.env`, pass 1 is the
disabled one and pass 2 is the enabled one. Either way the side-by-side block
at the end has both results.

The payload route is unchanged and stays as the negative control (it answers
nothing about GPU memory):

```sh
# 3. C4 fallback: ~1 s inference, writes photo.psplat next to the photo.
platform/ps5/bench/run-bench.sh --payload da2_bench 300

# 4. SHARP: 1.25 GiB model, all stages, writes photo.sharp.psplat.
platform/ps5/bench/run-bench.sh --payload sharp_bench 900
```

`run-bench.sh <name> [seconds]` (no flag) is still the payload route it was in
the previous revision: it picks the `.stripped.elf`, sets `PS5_RUN_READY` and
`PS5_HTTP_TIMEOUT`, and hands over to `run.sh`. `PS5_HTTP_TIMEOUT=300` covers
the 67.9 MiB upload on the first deploy of each ELF (the default is 30 s;
`vk_hello`'s 96.8 MiB needed 180 s). `run.sh`'s second argument is the klog
capture window; the start line appears within seconds, so a larger window only
keeps listening. Captures land in `$SPLAT_DATA_DIR/runs/console/<UTC>-<name>.log`.

`PS5_RUN_READY` is set on every command because `run.sh` matches the **ELF
stem** against the payload's first line by default, and the deployable files are
`<name>.stripped.elf` (stem `op_audit.stripped`, which is not what the payload
prints). The pattern `op_audit:` matches the start line and every later line. If
`run.sh` still reports no start line, deploy once by hand with
`platform/ps5/scripts/deploy.sh` and read `klog.sh` — the payload is running and
its log is being captured; only the readiness match failed.

### 7.1 Expected log lines

klog prefixes every line with the process name; the payload's own lines are the
rest. The title route adds the dispatcher's lines and the pool lines before
each block below (`[PPSA99201]` is the title process; the payload's
`MESA_SHADER_CACHE_DISABLE=true` line is absent there, because a title has a
folder for RADV's cache). `op_audit` (abridged, title route):

```
bench_title: start (ps5-splat-vr Spike A benches, one homebrew title, RADV linked directly)
bench_title: driver stderr capture on
bench_title: bench op_audit (from /data/ps5-splat-vr/bench/bench.env)
op_audit: direct pool 12884901888 bytes (12.00 GiB), available … rv=0x00000000
op_audit: flexible pool 448 MiB configured rv=0x00000000, … MiB free
op_audit: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)
op_audit: start (ps5-splat-vr Spike A op audit, Vulkan vs console CPU)
op_audit: coopmat at the driver default (GGML_VK_DISABLE_COOPMAT=(unset))
op_audit: ggml debug: ggml_vulkan: 0 = <RADV device> | uma: 1 | fp16: 1 | ... | warp size: 64 | shared memory: 65536 | ... | matrix cores: <coopmat1|coopmat2|none>
op_audit: device <RADV device name>
op_audit: 35 cases
op_audit: reference sharp.stem_16x16s16_3to1024_384 on the CPU
...
op_audit: ---- pass driver-default (coopmat on) ----
op_audit: case sharp.stem_16x16s16_3to1024_384 (exact, big)
op_audit: PASS sharp.stem_16x16s16_3to1024_384 coopmat=on vk <ms> ms cpu <ms> ms nodes 1 max_abs <…> nmse <…> cos <…> max_ref 12.4
...
op_audit: stem parity coopmat=on: F32 im2col nmse <…>, ggml_conv_2d nmse <…> (<N>x worse) -> use F32 im2col
op_audit: pass driver-default: ok=35 fail=0 unsupported=0 in <s> s
op_audit: splitting: 35 cases, max 8 nodes per graph, 1 submit each (below the 64-node GraphSession budget)
op_audit: ---- pass coopmat-disabled (coopmat off) ----
op_audit: PASS sharp.stem_16x16s16_3to1024_384 coopmat=off vk <ms> ms cpu <ms> ms nodes 1 ...
...
op_audit: pass coopmat-disabled: ok=35 fail=0 unsupported=0 in <s> s
op_audit: ---- coopmat on/off, side by side (pass 1 on, pass 2 off) ----
op_audit: sharp.stem_16x16s16_3to1024_384             PASS    <ms> ms nmse <…>  | PASS    <ms> ms nmse <…>
...
op_audit: summary ok=70 fail=0 unsupported=0 in <s> s
op_audit: peak RSS <…>
op_audit: PASS
op_audit: done, exit 0
```

If the driver has no cooperative matrix the two `---- pass` lines collapse into
one and the payload logs `coopmat on/off: the driver exposes no cooperative
matrix ...; single pass`. On a driver that does expose it, the second pass also
prints the `matrix cores:` line again (a new `VkDevice`).

`da2_bench`:

```
da2_bench: start (ps5-splat-vr Spike A, C4 fallback end to end)
da2_bench: photo /data/ps5-splat-vr/bench/photo.jpg
da2_bench: gguf /data/ps5-splat-vr/models/dav2s-f16.gguf
da2_bench: decoded 1280x873 in <s> s (3.20 MiB)
da2_bench: focal <px> px (EXIF)
da2_bench: device <RADV device name>
da2_bench: heap before load: <free> free of <total>
da2_bench: model loaded in <s> s (depth-anything-v2-small)
da2_bench: heap after load: <free> free of <total>
da2_bench: stage preprocess               <s> s  (overall 8%)
da2_bench: stage patch embed              <s> s  (overall 18%)
da2_bench: stage encoder                  <s> s  (overall 50%)
da2_bench: stage dpt                      <s> s  (overall 78%)
da2_bench: stage splat grid               <s> s  (overall 100%)
da2_bench: inference <s> s, <n> submits, 499896 splats
da2_bench: peak RSS after inference <…>
da2_bench: wrote /data/ps5-splat-vr/bench/photo.psplat (13.35 MiB)
da2_bench: heap after run: <free> free of <total>
da2_bench: PASS
da2_bench: done, exit 0
```

`sharp_bench` (the stage names come from the backend; the summary is the
authoritative per-stage breakdown):

```
sharp_bench: start (ps5-splat-vr Spike A, SHARP stages as built)
sharp_bench: photo /data/ps5-splat-vr/bench/photo.jpg
sharp_bench: gguf /data/ps5-splat-vr/models/sharp_2572gikvuh-f16-v1.gguf
sharp_bench: decoded 1280x873 in <s> s
sharp_bench: focal <px> px (EXIF)
sharp_bench: device <RADV device name>
sharp_bench: heap before load: <free> free of <total>
sharp_bench: model loaded in <s> s (sharp, 5.00 GiB needed)
sharp_bench: stage preprocess               <s> s  (overall 4%)
sharp_bench: stage pyramid                  <s> s  (overall 8%)
sharp_bench: stage patch encoder            <s> s  (overall 43%)
sharp_bench: stage encoder                  <s> s  (overall 66%)
sharp_bench: stage depth decoder            <s> s  (overall 76%)
sharp_bench: stage initialiser              <s> s  (overall 78%)
sharp_bench: stage Gaussian decoder         <s> s  (overall 93%)
sharp_bench: stage composer                 <s> s  (overall 100%)
sharp_bench: timings preprocess <s> s, encoder <s> s, depth <s> s, initialiser <s> s, gaussians <s> s, composer <s> s, total <s> s
sharp_bench: inference <s> s, <n> submits, 1179648 splats, focal <px> px
sharp_bench: peak RSS after inference <…>
sharp_bench: heap after run: <free> free of <total>
sharp_bench: wrote /data/ps5-splat-vr/bench/photo.sharp.psplat (31.50 MiB)
sharp_bench: PASS
sharp_bench: done, exit 0
```

The stage fractions and the splat counts are the desktop values; the console
scales the times, not the shapes. `submits` is the number of
`ggml_backend_graph_compute` calls the link saw (one per `GraphSession` submit):
28 for a DA2 run at 924×518 on the desktop, tens for SHARP. `op_audit` does not
use `GraphSession`; it logs `nodes` per case and the max node count in the
splitting line (8 today), which is the evidence that one submit per case is
already below the 64-node budget.

### 7.2 Failure table

| Symptom | Meaning | Next step |
| --- | --- | --- |
| `run.sh` exit 3 | no klog stream: ProsperoMgr/klogsrv down | open `http://$PS5_HOST:7070`, check `/api/elfldr/status`; `klog.sh --ensure` |
| exit 4 | deploy failed: ProsperoMgr refused or elfldr on its loopback is down | check the `deploy\|` lines; `probe_agc.elf` first if nothing has run yet |
| exit 5, zero bytes | connected but no log line: payload died before its first write | check `/api/processes` for `payload.elf`; retry with `klog.sh` connected first |
| exit 1, no start line | loader rejected the ELF or it died before `main` (missing NEEDED module) | `probe_agc.elf`; if that fails too, the AGC/VideoOut imports are the cause and the big ELFs will fail the same way |
| start line, then nothing | crash between `main` and the first line (heap/thread init) | look for `[elfldr.elf]`/process-exit lines |
| `no Vulkan device (RADV init failed…)` | `ggml_backend_vk_reg()` caught an exception; the `[radv]` lines say which call | record the `[radv]` line; `vk_hello.elf` isolates driver init from ggml |
| `heap before load:` ~7.6 GiB, or two heaps | `radv_enable_unified_heap_on_apu` did not reach RADV | the payloads set it before the first GPU call; record both heap lines |
| `matrix cores: coopmat1` and a conv case fails only in the `coopmat=on` pass | RADV's cooperative-matrix path accumulates in f16 | the side-by-side block names both passes; record both in `docs/research/op-audit.md` — no second run needed |
| `coopmat on/off: ... single pass` | driver has no cooperative matrix, or `bench.env` disabled it, or the build has no device-reset glue | for a driver with none that is the answer; otherwise check that `vk_reset.cpp` is in the link (it is, for the PS5 payloads) |
| `WARNING coopmat state did not change between passes` | the released device was rebuilt but the env override did not take effect | record it; the two passes are not a comparison on that driver |
| `compute error <case>: FAILED` then `leaking ... exiting 1` | device lost / rejected submission; the process deliberately frees nothing | record the case, the last `[radv]` lines and the submit count; that is the Spike A device-lost evidence |
| `UNSUP <case>` | `ggml_backend_dev_supports_op` rejected an op at that shape | the op/shape is printed; a port decision, not a driver fault |
| `PASS` everywhere but `stem parity … either is inside the gate here` | the two patch-embed paths agree on this device | still prefer F32 im2col; note the ratio |
| `WATCHDOG <stage> exceeded its deadline` | a submission never signalled (device lost or wedged) | record the stage, the last `[radv]` lines and the submit count; set `GGML_VK_MAX_NODES_PER_SUBMIT=64` and retry once |
| `no photo in …` / `no GGUF at …` | data not pushed | `push-data.sh` then `--verify` |
| `cannot decode …` | JPEG/PNG not decodable by stb | use a different photo; `PUSH_PHOTO=… push-data.sh` |
| `model loaded in` then a stage fails | weight/layout mismatch (wrong GGUF) | `splat_convert --backend da2|sharp` on the console-side file, or re-push |
| `wrote … .psplat` missing / write error | `/data` full or not writable | check the free space in ProsperoMgr's Files tab; nothing is deleted by these tools |
| `peak RSS` near the heap figure | activation memory is the limit, not the model | record it; the spec's 8 GiB SHARP budget is the gate |
| title: `bench_title: no BENCH= in …` | `bench.env` was not written (launched by hand or by the tile) | `run-bench.sh --title <bench>` writes it; put `BENCH=…` there by hand otherwise |
| title: `op_audit: direct pool 0 bytes` | the process did not get the app pool — the one result that sinks the title route | record it; re-read `spike-b-memory.md` §1-2 before trusting the bench for memory |
| title: `run-bench: no end line … in Ns` | a wedged submission or a driver hang; the watchdog should have fired | look for `WATCHDOG` in the capture; `title.sh stop` ends the app through the quit file |
| title: `run-bench: launch failed (title.sh exit N)` | not registered / no foreground user / another title running | follow `title.sh`'s own message; `title.sh status` first |
| title: `bench_title: … done, exit N` with no `PASS` | the bench returned a failure code (2 setup, 1 run) | the `FAIL` line above names it; see the rows above |
| title: `quit requested` | someone ran `title.sh stop` while the bench was in flight | expected on a manual stop; the `.psplat` is not written |

### 7.3 What the console answered (2026-10-26, session 2)

All three benches as the title `PPSA99201` on firmware 13.60, RADV NAVI21.

| Bench | Pool | Result | Key numbers |
| --- | --- | --- | --- |
| `op_audit` | `direct pool 12280922112` (11.44 GiB), `1 heap(s), 6 type(s)` | **PASS** | `matrix cores: none` → one pass only: `pass driver-default: ok=35 fail=0 unsupported=0 in 8.1 s`, `summary ok=35 fail=0 unsupported=0`, `peak RSS 6.25 GiB` |
| `da2_bench` | same 11.44 GiB heap; 11.40 GiB free before load, 11.19 after | **PASS** | `inference 1.01 s, 19 submits, 477998 splats`; `wrote photo.psplat (12.76 MiB)`; `peak RSS 742.95 MiB` |
| `sharp_bench` | 11.40 GiB free before load; 8.95 after the 5.00 GiB model; 4.87 after the run | **PASS** | `timings preprocess 0.02, encoder 4.16, depth 1.19, initialiser 0.03, gaussians 1.30, composer 5.09, total 11.79 s` (gate 60 s; desktop reference 49.6 s); `159 submits, 1179648 splats`; `peak RSS 7.22 GiB`; `wrote photo.sharp.psplat (31.50 MiB)` |

`radv_enable_unified_heap_on_apu=true` reached RADV in every run (`[radv]ATTENTION … overridden by environment`), and the title process always got the app pool — the payload route's `memory: 0 heaps` does not happen here. SHARP's model load (14.60 s) is outside the 11.79 s stage total. The `direct pool` is 11.44 GiB, not the 12.00 GiB in §7.1; the heap is one device-local heap over it, as expected.

## 8. Files written

| File | What |
| --- | --- |
| `platform/ps5/ggml/CMakeLists.txt` | cross build: ggml CPU+Vulkan, core sources, miniz, glue |
| `platform/ps5/ggml/ps5-toolchain.cmake` | SDK + RADV paths, host glslc/SPIRV-Headers, static try_compile |
| `platform/ps5/ggml/host-toolchain.cmake` | host toolchain for `vulkan-shaders-gen` |
| `platform/ps5/ggml/cmake/vulkan/VulkanConfig.cmake` | stand-in Vulkan package: headers, no library |
| `platform/ps5/ggml/ps5_vulkan.cpp` | the four Vulkan entry points + `fork`/`execlp`/`pathconf` |
| `platform/ps5/ggml/vk_reset.cpp` | releases ggml-vulkan's cached `VkDevice` between `op_audit`'s coopmat passes (breaks its two self-references, then destroys it) |
| `platform/ps5/ggml/bench_env.h/.cpp` | bench.env, logging, heap/RSS, submit counter, watchdog |
| `platform/ps5/ggml/ggml_logging.h` | ggml log lines → klog (device/coopmat line) |
| `platform/ps5/ggml/build.sh` | configure + build, `LLVM_CONFIG` handling |
| `platform/ps5/ggml/README.md` | the build route and why each glue piece exists |
| `platform/ps5/bench/op_audit.cpp` | the audit bench (35 cases, both coopmat passes); payload `main` + `op_audit_run` for the title |
| `platform/ps5/bench/da2_bench.cpp` | the C4 bench; payload `main` + `da2_bench_run` |
| `platform/ps5/bench/sharp_bench.cpp` | the C3 bench; payload `main` + `sharp_bench_run` |
| `platform/ps5/bench/bench_common.h` | shared photo discovery, stem/dir and model path helpers (`splat::Path`) |
| `platform/ps5/bench/bench_title.cpp` | the one title's program: `BENCH=` selector, pool log, dispatch, quit watch, `LoadExec` end |
| `platform/ps5/bench/bench_title.h` | title-mode log/pool/ggml helpers, RADV defaults per mode, `abandon()` |
| `platform/ps5/bench/bench_dispatch.h` | the three entry points the title calls |
| `platform/ps5/bench/Makefile` | whole-archive RADV link recipe + `strip` target + `bench-title` (title package) |
| `platform/ps5/title/param-bench.json` | the bench title's `param.json` (`PPSA99201`) |
| `platform/ps5/title/build-title.mk` | `TITLE_PARAM_JSON`, `TITLE_EXTRA_LIBS`/`TITLE_EXTRA_LDFLAGS`: more than one program per recipe |
| `platform/ps5/title/README.md` | the second program row |
| `platform/ps5/scripts/title.sh` | `put LOCAL REMOTE` (the bench selector file), additive |
| `platform/ps5/bench/run-bench.sh` | `--title` route: selector, install-if-needed, launch, klog to end line, stop; `--payload` unchanged |
| `platform/ps5/radv-link.mk` | the shared RADV/`--wrap`/`--defsym` link lists (bench + vk_hello) |
| `platform/ps5/bench/verify-elfs.sh` | static checks, reusing Spike B's checker |
| `platform/ps5/scripts/push-data.sh` | data transfer (ProsperoMgr chunk route, resumable, `--dry-run`) |
| `docs/research/spike-a.md` | this file |

Outside the repo: `build-F6-spikea/` (gitignored build dir),
`build/ps5/title/PPSA99201/` (the bench title package, gitignored),
`$SPLAT_DATA_DIR/runs/A5/` (`build-clean.log`, `build-payloads.log`,
`verify-*.txt`), the F6 re-run logs (`F6_ggml_build.log`,
`F6_payloads_build.log`, `F6_verify*.txt`, `F6_host_*.log`), and the console
captures `runs/console/…` from tomorrow.

Follow-ups: add the payload row to
`platform/ps5/README.md`'s "What this project has put on the console" after the
run (outside this task's area), and the two `docs/BACKLOG.md` lines for the
ggml fence-timeout patch and the host smoke build's `--wrap` gap. One more line
for `docs/BACKLOG.md`: a title-end hook in `platform/ps5/ggml/bench_env.cpp`
for the watchdog's `_exit(3)` (§2.4, §5).

## Sources

- `docs/spec.md` — C2, C3, C4, budgets, risks, milestones; `docs/PLAN.md`
- `docs/research/spike-b-memory.md` — why a payload has 0 B of direct memory,
  the app pool a title gets, the title dev loop and the graceful quit
- `platform/ps5/title/{README.md,build-title.mk,param.json}` — the title
  package, the CRT/converter/signer route and the provisional title ids
- `$SPLAT_DATA_DIR/ref/PS5LM/ps5/app/main.cpp` — the quit file and
  `sceSystemServiceLoadExec("exit", nullptr)` the stop route follows
- `docs/research/spike-b.md` — the RADV build, the whole-archive link recipe,
  the unified-heap setting, bounded teardown
- `docs/research/op-audit.md` + `tools/op-audit/` — the cases, the F16-im2col
  trap, the coopmat/f16-accumulation question this payload re-runs on RADV
- `core/recon/{ggml,sharp,fallback}/README.md` — the graphs, the submit split,
  the measured parity and cost
- `platform/ps5/{README.md,vk_hello/,radv/}` — the payload loop and link recipe
- `$SPLAT_DATA_DIR/ref/ProsperoMgr/source/backend/src/api_routes.cpp` — the
  `/api/ftp/upload/chunk` contract (264 MiB cap, `next_offset`, rename on
  completion) and `/api/ftp/mkdir`
- `$SPLAT_DATA_DIR/ref/ftpsrv/README.md` — the fallback route (port 2121)
