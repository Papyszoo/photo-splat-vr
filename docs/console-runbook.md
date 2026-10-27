# Console runbook — first hardware session

From the repo root, one command at a time, foreground. Console awake, ProsperoMgr running, headset connected; addresses and ports come from the gitignored `.env.local`. Sources: `docs/research/spike-b-memory.md` §§5-7, `spike-a.md` §7, `spike-b.md` §7, `spike-b-hmd2.md` §8, `ps5-console.md` §8.

`run.sh`/`run-bench.sh --payload` exit **0 when the payload's start line appeared**, not when it passed — judge by the payload's own `PASS` line. 1 no start line, 2 usage, 3 no log stream, 4 deploy/upload failed, 5 the capture stayed empty.

`run-bench.sh --title` (the bench title, step 7) exits **0 when the bench's `done, exit` line appeared**, not when it passed — judge by its `PASS` line. 1 the end line did not appear in the window, 2 usage or missing package, 3 no log stream, 4 put/install/launch failed, 5 the capture stayed empty or the title would not stop.

`run-viewer.sh` (the viewer title, step 1b) has the same codes and the same rule: **0 means `viewer: done, exit` appeared**, and the picture is judged by the `t=…s fps=…` lines and by looking at the TV (`PASS` is the app's own verdict).

`run-vr-hello.sh` (the vr_hello title, step 1c) has those codes too: **0 means `vrhello: done, exit` appeared**; the headset picture is judged by the `t=…s` lines and by looking through the headset (`PASS` is the app's own verdict).

`convert-inbox.sh` (step 7b) exits **0 when the title's own `convert: PASS` line appeared** and the viewer is back, 1 when it did not (no end line, or the title's FAIL), 2 usage/missing or unsafe photo/a refusal to overwrite an inbox photo, 3 no log stream, 4 build/put/install/launch failed, 5 empty capture or the title did not stop, 6 the viewer build or `viewer.env` could not be restored.

`title.sh` has its own codes: 0 done, 1 a request failed, 2 a bad argument, an unsafe path or an unusable package, 3 no ProsperoMgr (install also: no ShadowMountPlus), 4 not registered / launch refused, 5 the title is running (or stayed running after `stop`).

## Two installs: dev `PPSA99202`, release `PPSA99770`

Every title package this repo builds — vk_hello (step 1), the viewer (1b, 1d),
vr_hello (1c) and the bench title (7) — is the **same title per identity**:
`PPSA99202` "Photo Splat VR Dev" for the dev build (the default, used everywhere
below) and `PPSA99770` "Photo Splat VR" for the release build
(`make … RELEASE=1`). The identity comes from
`platform/ps5/title/identities.json` (the title name, the PS VR2 attributes, the
package's `sce_sys/param.json` generated from the identity-free template, and
the data folder the build compiles in), so the console keeps one tile per
identity and every install updates that tile in place.

**The two installs share nothing.** The dev build keeps `/data/ps5-splat-vr`
(every path and log excerpt below uses it); the release build uses the user's
private `/data/photo-splat-vr` and downloads its own models. No script in this
repo may read, list, write or delete under `/data/photo-splat-vr`, and no agent
script may launch `PPSA99770` (AGENTS.md). `ps5-env.sh` enforces both before a
request is built: `ps5_agent_guard_identity` refuses `PPSA99770` for
`ui-test.sh`, `run*.sh`, `push-data.sh`, `convert-inbox.sh`, `spike.sh` and
`title.sh launch/stop/put`, and `ps5_guard_remote_path` refuses the private data
folder whatever identity is selected. `title.sh install/status` still work for
the release package: they touch `/data/homebrew/PPSA99770` only, and installing
its files is allowed. `title-uninstall.sh --data PPSA99770` is refused.

`fresh` (`PPSA99771` "Photo Splat VR Fresh Test", data `/data/psvr-fresh-test`, `make … IDENTITY=fresh`) is the throwaway slot for the pre-release fresh-install check — install the release zip there, run the first-use check, then `title-uninstall.sh --yes --data PPSA99771`, so it is gone after each check (the guards allow it like dev; `PPSA99770` and `/data/photo-splat-vr` stay refused).

Within one identity, nothing else needs deleting or reinstalling per test, but an install of
one program replaces whatever eboot was there: after a bench or `run-vr-hello.sh`
run, the tile starts that program until you install the viewer again
(`run-viewer.sh` does it, because its `title.sh status` sees the size mismatch).
`run-bench.sh --title` and `run-vr-hello.sh` print that reminder. The PS VR2
attributes (`attribute 0x62000000`, `attribute3 0x4080c40`) are on the package
for every program; only a program that opens Hmd2 acts on them, so vk_hello and
the benches are unaffected (`docs/research/hla-psvr2.md` §4).

The log excerpts below were captured under the earlier dev title id and are
re-tagged to `PPSA99202`; the lines are otherwise verbatim.

## Preflight (2 min, writes nothing to the console)

- Console on, **not in rest mode**: Settings → System → Power Saving → Features Available in Rest Mode → both options *off*; Time Until PS5 Enters Rest Mode → *Don't Put in Rest Mode*. Rest mode kills every payload.
- After a boot or wake, re-run the autoloader (Y2JB) first: elfldr and ProsperoMgr are payloads and do not survive a reboot. The title route needs, in this order, kstuff(-lite) → a HEN (OnionHEN/etaHEN) → **ShadowMountPlus** → ProsperoMgr. ShadowMountPlus is what turns `/data/homebrew/<TITLE_ID>` into a home-screen tile; without it `title.sh install` stops at the scan request.
- A **foreground user signed in** on the console: `sceSystemServiceLaunchApp` takes that user, and an app started without one is ended after ~20 s.
- Headset connected and powered, on your head only for step 5; no other VR app.
- Log server + dashboard in one command:

  ```sh
  platform/ps5/scripts/klog.sh --ensure      # klog: log stream ready on <host>:3232
  set -a; . ./.env.local; set +a
  curl -s "http://$PS5_HOST:7070/api/elfldr/status"    # {"reachable":true,"error":""}
  curl -s "http://$PS5_HOST:10101/api/v1/version" -H 'Content-Type: application/json' -d '{}'
  ```

  If the first two fail, stop: ProsperoMgr (7070) or elfldr is down and no payload step works. If the third fails, the title route cannot register anything: no title step works either.

## The run, in this order

Each payload command prints `run: log <path>` and writes `$SPLAT_DATA_DIR/runs/console/<UTC>-<stem>.log`, stem = ELF name minus `.elf` (bench logs are `…-op_audit.stripped.log` etc.). `run-bench.sh --title` writes `<UTC>-<bench>-title.log`; a manual `title.sh launch` writes no file, so capture the klog with `klog.sh <file>` in a second terminal (connected before the launch), or read ProsperoMgr's Logs page.

### 1. Title gate — vk_hello as a title — first, and the gate for everything GPU

Build on the Mac (a few seconds), then read the request list before sending it:

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/title                    # build/ps5/title/PPSA99202/ (25 MiB eboot.bin)
platform/ps5/scripts/title.sh install --dry-run build/ps5/title/PPSA99202
```

Then install, with klog already streaming in another terminal (`platform/ps5/scripts/klog.sh`):

```sh
platform/ps5/scripts/title.sh install build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh launch  build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh status  build/ps5/title/PPSA99202
```

- `install` prints `files verified: 4 file(s) under /data/homebrew/PPSA99202`, then `shadowmount scan: HTTP 200`, then `registered: PPSA99202 is in the title list`. It waits up to 90 s (`PS5_TITLE_WAIT`). The upload is ~25 MiB.
- `launch` prints `running: PPSA99202`. klog then shows `launchApp(PPSA99202)`, `EXEC /app0/eboot.bin` and the app's own lines.
- Success (klog; the prefix is the process name, the message text is what to match — it is the same text the payload prints):

  ```
  <118>[PPSA99202] vk_hello: start (ps5-splat-vr, title build, RADV linked directly, no loader)
  <118>[PPSA99202] vk_hello: direct pool 12884901888 bytes (12.00 GiB), available … rv=0x00000000
  <118>[PPSA99202] vk_hello: flexible pool 448 MiB configured rv=0x00000000, … MiB free
  <118>[PPSA99202] vk_hello: firmware 13.60 (kern.sdk_version 0x13600007)
  <118>[PPSA99202] vk_hello: instance created (Vulkan 1.1 request, no extensions)
  <118>[PPSA99202] vk_hello: memory: 1 heaps, N types
  <118>[PPSA99202] vk_hello: heap[0] 11.44 GiB (NNNN bytes) flags 0x1 device-local
  <118>[PPSA99202] vk_hello: heap layout: 1 heap(s), N type(s), device-local 11.44 GiB in 1 heap(s)
  <118>[PPSA99202] vk_hello: fill verified: 262144/262144 elements == i*3+7
  <118>[PPSA99202] vk_hello: PASS
  <118>[PPSA99202] vk_hello: done, exit 0
  ```

  The gate is the three numbers, not the wording: **`direct pool` ~12 GiB**, **`heap layout: 1 heap(s) … device-local 11.44 GiB`** (11.4-12 GiB; 11712 or 12288 MiB are both possible and both fine), and **`PASS`** with `fill verified`. Then the title registers, mounts and runs — the whole route — and the GPU steps below can be believed.
- If it fails, in order of likelihood:
  - `title.sh install` exit 3 at the scan: ShadowMountPlus is not running (start it, then re-run install).
  - `install` exit 4 after 90 s: files are on the console but not registered. ShadowMountPlus's debug log saying `Register failed: TitleDir bridge unavailable` means its ShellCore hook is stale — reload the payload. Otherwise the id may already belong to something.
  - `launch` exit 4: no foreground user, or the title is not in `GET /api/titles`.
  - klog shows `launchApp` + `EXEC` but no `vk_hello:` line: the title started and died before `main` (a missing module or the `libc.prx` companion). `title.sh stop` to clear it, reinstall, and read the `[radv]` / loader lines.
  - `direct pool` is small or 0: this process did not get the app pool — that is the one result that would sink the title route; record the number.
  - `memory: 0 heaps`: the direct pool is 0 or RADV did not see it; check the `direct pool` line above it.
  - two heaps (~7.6 + ~3.8 GiB): `radv_enable_unified_heap_on_apu` did not reach RADV.
  - `FAIL vkQueueSubmit` / `vkWaitForFences`: device lost; the runbook stops here as it would for a payload.
- `/data` being writable from a title is still unproven: this probe only logs and writes nothing. The first title that writes (`/data/ps5-splat-vr/…`) answers it. If it cannot, add `PPSA99202` to the HEN's app-jailbreak allowlist (`/data/OnionHEN/config.ini`, `[app_jailbreak] exact_title_ids`) and start the app again (PS5LM documents exactly this step).
- Leaving it: the title exits itself (`LoadExec("exit")`) after PASS, so nothing needs stopping. `title.sh stop` is for a title that is still running; it never kills.

### 1b. Splats on the TV — the viewer title — the first visible result

The viewer title (`platform/ps5/viewer/`, the one app `PPSA99202`) loads a
`.psplat` scene from `/data/ps5-splat-vr/scenes/`, renders it with the project's
own renderer (C6: `SplatRenderer`, `ViewerState`, comfort fade) at 1920x1080 and
presents each frame through VideoOut, mono, with a slow side-to-side sway inside
60 % of the comfort radius. It is the same title process step 1 proved, so run it
**after** step 1 passed: that is where `direct pool` ~12 GiB, `heap layout: 1
heap(s)` and `PASS` came from.

**The same title's default is now VR mode** (step 1d): this step needs
`--mode tv`, which writes `MODE=tv` to `viewer.env`. The package's `param.json`
carries the PS VR2 identity, so the TV path under it is not proven yet
(`docs/BACKLOG.md`).

Build on the Mac (one cross build once, then the package), then read the plan:

```sh
platform/ps5/ggml/build.sh                     # ggml + core, ~5 min the first time
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/viewer                    # build/ps5/title/PPSA99202/ (26.6 MiB)
platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99202   # static checks
platform/ps5/scripts/run-viewer.sh --dry-run   # every request, sends nothing
```

Push a scene if `/data/ps5-splat-vr/scenes/` is empty (the viewer logs
`scenes: 0 folder(s)` and exits 2 if it is). `--scenes DIR` takes every
immediate subfolder that holds `scene.psplat`, `--scene DIR` one folder; either
pushes only scenes:

```sh
platform/ps5/scripts/push-data.sh --check --scenes "$SPLAT_DATA_DIR/runs/render/comfort/scenes"
platform/ps5/scripts/push-data.sh --scenes "$SPLAT_DATA_DIR/runs/render/comfort/scenes"
```

Then one command (klog is started by the script, so it is connected before the
launch; it installs the package when `title.sh status` does not verify it):

```sh
platform/ps5/scripts/run-viewer.sh --mode tv                    # 60 s, first scene by name
platform/ps5/scripts/run-viewer.sh --mode tv --scene 02_living_lite --seconds 120
```

- `run-viewer.sh` writes `/data/ps5-splat-vr/bench/viewer.env` (`MODE=tv`,
  `SECONDS=`, `SCENE=`) with `title.sh put`, installs if needed, launches, captures
  `$SPLAT_DATA_DIR/runs/console/<UTC>-viewer-title.log`, and stops the title
  when `viewer: done, exit` appears (after a normal end there is nothing to
  stop). Exit: 0 end line seen, 1 not in the window, 2 usage/missing package,
  3 no log stream, 4 put/install/launch failed, 5 empty capture or the title did
  not stop.
- What is on screen: the first scene's capture view, swaying ±15 cm (60 % of
  the 0.25 m comfort radius) with a 12 s period, no stereo, no head tracking.
  At 60 % the comfort fade is 0.896, so the picture dims very slightly at the
  ends of the swing; past the photo's edge the capture-frustum fade blends into
  the 0.18 grey backdrop. `viewer: t=…` prints `fade=` and `head_x=` each
  second, so this is verifiable from the log alone. A capture that is not 16:9
  (1280x873, 1920x1440) is stretched to the 1920x1080 buffer exactly as
  `splat_desktop --size` scales its intrinsics; letterboxing is a later item.
- `PROBE=sources` in `viewer.env` (e.g. `--env` a file holding it) runs the T7
  source spike before the first GPU call: `opendir` + entry count for `/user`,
  `/user/av_contents`, `/user/av_contents/photo` (3 levels, 200 entries), the
  ShadowMountPlus alias roots `/mnt`, `/mnt/usb0..7`, `/mnt/ext0..1`,
  `/mnt/disc` (one level down), `/app0/..`, `/app0/../mnt`, `/data`,
  `/data/ps5-splat-vr` and `/data/ps5-splat-vr/inbox`, plus a 16-byte read of
  the first jpg/jpeg/png/jxr found (errno or 4-byte magic only); the viewer then
  continues normally. Answers: `docs/research/browse-ui.md` §1.3-§1.5.
- Success (klog; the prefix is the process name, the text after it is what to
  match):

  ```
  <118>[PPSA99202] viewer: start (ps5-splat-vr, C6 renderer, RADV linked directly, no loader)
  <118>[PPSA99202] viewer: direct pool 12884901888 bytes (12.00 GiB), available … rv=0x00000000
  <118>[PPSA99202] viewer: flexible pool 448 MiB configured rv=0x00000000, … MiB free
  <118>[PPSA99202] viewer: firmware 13.60 (kern.sdk_version 0x13600007)
  <118>[PPSA99202] viewer: config /data/ps5-splat-vr/bench/viewer.env: scene=(first) seconds=60 out=1920x1080 flip_rate=1 comfort_radius=0.250 m sway_period=12.0 s
  <118>[PPSA99202] viewer: radv_enable_unified_heap_on_apu=true (one heap over the direct-memory pool)
  <118>[PPSA99202] viewer: driver stderr capture on
  <118>[PPSA99202] viewer: scenes: 2 folder(s) under /data/ps5-splat-vr/scenes
  <118>[PPSA99202] viewer: scene 1/2 01_living: 1179648 splats, load 268.0 ms
  <118>[PPSA99202] viewer: capture intrinsics fx 1074.29 fy 1074.29 cx 640.0 cy 436.5, image 1280x873
  <118>[PPSA99202] viewer: device PlayStation 5 GPU, Vulkan 1.4.x, driver 0x…
  <118>[PPSA99202] viewer: memory: 1 heaps, N types
  <118>[PPSA99202] viewer: heap[0] 11.44 GiB (…) device-local
  <118>[PPSA99202] viewer: device up in … ms
  <118>[PPSA99202] viewer: videoout: 1920x1080, 2 buffer(s) of 10.00 MiB, flip rate 1
  <118>[PPSA99202] viewer: renderer: 1920x1080 mono, shaders /app0/shaders, create … ms
  <118>[PPSA99202] viewer: t=1.0s fps=… project=… sort=… draw=… ms copy=… tiles=… flip=… ms visible=… fade=1.000 head_x=+0.021 m
  …
  <118>[PPSA99202] viewer: run time 60 s reached (60 s)
  <118>[PPSA99202] viewer: totals: … frames in 60.0 s (… fps), GPU … ms/frame, copy … ms/frame, tiles … ms/frame, present … ms/frame
  <118>[PPSA99202] viewer: teardown … ms
  <118>[PPSA99202] viewer: PASS, exit 0
  <118>[PPSA99202] viewer: done, exit 0
  ```

  The numbers to read: `scenes: N folder(s)`, the `scene … splats, load … ms`
  line, `videoout: 1920x1080`, a moving `fps=` with a non-zero `draw=` (when
  the device reports timestamps; otherwise the line after `renderer:` says they
  are unavailable and `copy=`/`tiles=`/`flip=` carry the timing), and `PASS`.
  `create … ms` on the first ever run includes ACO compiling the six shader
  pipelines (seconds, not milliseconds); RADV's cache under
  `/app0/radv-shader-cache` makes the second run fast if that folder is
  writable.
- If it fails, in order of likelihood:
  - `FAIL scenes: no scene folders under /data/ps5-splat-vr/scenes` then
    `FAIL no scenes, exit 2`: push a scene (above). The home screen comes back;
    `SCENES_DIR=` in viewer.env can point at a single scene folder instead.
  - `FAIL shaders: no project.comp.spv in /app0/shaders or
    /data/ps5-splat-vr/shaders`: the package's `shaders/` did not arrive —
    `title.sh status` would have shown a `DIFF`; reinstall the package.
  - `FAIL videoout: sceVideoOutOpen -> 0x…`: the title could not open the TV's
    output (another title on screen, or the system refused the display to this
    process). `title.sh stop`, close anything running, relaunch.
  - `FAIL videoout: sceVideoOutRegisterBuffers2 -> 0x…`: the direct-memory
    framebuffers were refused (pool nearly empty, or the attribute is wrong).
    The `direct pool`/`heap[0]` lines above say whether memory was there; a
    second title holding the display gives `0x802900…`.
  - `FAIL render`/`FAIL frame copy` with `vkWaitForFences -> VK_TIMEOUT` or
    `DEVICE_LOST`: the GPU did not finish a frame. Teardown then logs
    `leaking the renderer, its buffers and the device` — that is correct, do
    not expect a clean `teardown … ms` line. Read the `[radv]` lines just above.
  - a frame on screen but the wrong picture (smear, offset, black bars): the
    tile layout or the buffer attribute, not the renderer — the shaders and the
    scene are the same ones the desktop viewer draws with (compare against a
    desktop `--screenshot`). Record it; `videoout.cpp` follows PS5_Mesa's WSI.
  - `fps=` far below ~25 with `draw=` in the tens of ms: GPU cost at 1080p (the
    desktop reference at 1280x873 is ~8.5 ms draw on an M4). Try a scene with
    fewer splats (`--scene`), `FLIP_RATE=0` for every vblank, or `WIDTH=2560
    HEIGHT=1440` on a 1440p screen; `tiles=` in the tens of ms points at the CPU
    tile write instead.
  - `viewer: present: a flip could not be confirmed shown within 500 ms;
    presenting unpaced` is a warning, not a failure: the display stopped pacing
    and the title tears rather than stalls.
- Leaving it: the title ends itself after `SECONDS` (60 s by default) through
  `LoadExec("exit")`. `title.sh stop` (which `run-viewer.sh` runs) writes the
  quit file for a run with `SECONDS=0`; it never kills.
- No input on the TV path: the DualSense Options button needs a user-service
  handle, which only the VR path's Hmd2 session opens (step 1d), so in `MODE=tv`
  the run length is the only stop. Recorded in `docs/BACKLOG.md`.

### 1c. The headset display path — vr_hello as a title — the PSVR2 picture

`vr_hello` becomes a title (`platform/ps5/vr_hello/`, the one app `PPSA99202`)
that renders a test pattern with RADV into three linear RGBA8 host-visible
buffers and flips them to the PSVR2 the way HLA-PSVR2 does: `param.json`
attribute `0x62000000` / attribute3 `0x4080c40`, `EnableVrMode(0x2000f)` and
`SetRenderConfig(1)` (60 rendered into 120 Hz), a three-vblank gate after VR
mode, and pacing on Hmd2's predicted display time. Per eye it uses HLA's
1984×2048 (side by side 3968×2048, 32.5 MB per image, 3 buffers). The pattern is
drawn by a Vulkan fragment shader into an image, copied to the mapped buffer on
the GPU, and waited on with a 500 ms fence — no CPU-authored pixels. Run it
**after** step 1 passed (the title route and the direct pool are what RADV
needs). Headset on and worn, on 13.60, nothing else using it.

Build, verify and read the plan (all on the Mac; nothing sent):

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/vr_hello vr-hello-title          # build/ps5/title/PPSA99202/
platform/ps5/vr_hello/verify-package.sh build/ps5/title/PPSA99202
platform/ps5/scripts/run-vr-hello.sh --dry-run        # every request, sends nothing
```

`verify-package.sh` must end `verify-package: PASS`: signed plaintext fSELF, the
title id and the VR identity in `param.json`, the published `libc.prx`, the
linked PIE needs VideoOut/SystemService/kernel, exports nothing, keeps the
Vulkan shims local, and carries both embedded SPIR-V pattern shaders. The
package is ~27 MB (`eboot.bin` 25.8 MB).

Then one command (it starts klog, installs if `title.sh status` does not verify
the package, launches, and stops a title that is still running):

```sh
platform/ps5/scripts/run-vr-hello.sh                   # 30 s run, 120 s window
```

- What you see: **left eye** a green background with a big **orange "L"**,
  **right eye** a blue background with a big **cyan "R"**; both a 128-pixel
  grid, a white centre cross, four white corner squares, a thin border, a
  coloured top band, and a **yellow vertical bar with a square knob** that moves
  left/right as you turn your head (half-way across the eye at 45°). The
  picture is a flat 2D pattern, so it is head-locked by construction; the bar
  moving is what shows the pose path is live. The TV keeps the console UI:
  vr_hello does not ask for Hmd2's mirror (the viewer's VR mode does, step 1d).
- Then: after 30 s the headset goes dark, the title tears Hmd2 down and ends
  itself through `LoadExec("exit")`; the home screen comes back.
- Success (klog; the kernel prefix is the process name, match the text):

  ```
  <118>[PPSA99202] vrhello: start (vr_hello title PPSA99202, RADV eye buffers, 30 s)
  <118>[PPSA99202] vrhello: firmware 13.60 (kern.sdk_version 0x13600007)
  <118>[PPSA99202] vrhello: requested mode 120 Hz output half-rate (EnableVrMode 0x2000f + SetRenderConfig(1)), eye 1984x2048, side-by-side 3968x2048, 3 buffers
  <118>[PPSA99202] vrhello: device PlayStation 5 GPU, Vulkan 1.4.x, …
  <118>[PPSA99202] vrhello: memory: 1 heaps, N types
  <118>[PPSA99202] vrhello: eye buffer 0: 3968x2048 RGBA8 at 0x… (32505856 bytes, memory type N)
  …
  <118>[PPSA99202] hmd2: user 1, eye 3968x2048 x3 buffers (external, linear RGBA8)
  <118>[PPSA99202] hmd2: eye buffer 2: 3968x2048 linear RGBA8 at 0x… (32505856 bytes)
  <118>[PPSA99202] hmd2: ReprojectionEnableVrMode -> 0x00000000, VideoOutOpen(0xff, bus 32) -> 0x2
  <118>[PPSA99202] hmd2: RegisterBuffers2 (3 x 3968x2048) -> 0x00000000
  <118>[PPSA99202] hmd2: ConfigureOutput (120 Hz) -> 0x00000000, SetRenderConfig(half rate) -> 0x00000000
  <118>[PPSA99202] hmd2: display live after 1100 ms (vblank rc 0x00000000 count 0 -> 3)
  <118>[PPSA99202] hmd2: ready (firmware table 1360, 21 exports)
  <118>[PPSA99202] vrhello: headset: per eye 1984x2048, side-by-side 3968x2048, 3 buffers, linear RGBA8 32505856 bytes each, refresh 120 Hz, fov out … in … top … bottom …
  <118>[PPSA99202] hmd2: Hmd2 live at frame 4 (display +12000 us)
  <118>[PPSA99202] vrhello: first frame in … ms (buffer 0, VkImage 3968x2048 R8G8B8A8_UNORM, GPU wait … ms)
  <118>[PPSA99202] vrhello: t=1s frames=60 fps=60.0 submitted=60 shown=58 missed=0 repeat=0 queue-full=0 err=0 pace[nap=0.20 call=1.10 ahead=9.50 step=16.67 ms] worn=1 lens=20 gpu=0.90 ms
  …
  <118>[PPSA99202] vrhello: run: 1800 frames in 30002 ms (60.0 fps), submitted=1800 shown=1798 missed=0 repeat=0 queue-full=0 err=0 flip-rc=0x00000000
  <118>[PPSA99202] vrhello: teardown: closing the VR display and Hmd2
  <118>[PPSA99202] hmd2: stopped after 1800 frames (unregister 0x00000000 close 0x00000000 disable VR mode 0x00000000)
  <118>[PPSA99202] hmd2: tracker down (…), reprojection down (…), sceHmd2Close(…) -> …, sceHmd2Terminate -> 0x00000000
  <118>[PPSA99202] vrhello: teardown: RADV objects destroyed
  <118>[PPSA99202] vrhello: teardown: done in … ms
  <118>[PPSA99202] vrhello: PASS 1800 frames, 60.0 fps, shown 1798, missed 0, headset worn
  <118>[PPSA99202] vrhello: PASS, exit 0
  <118>[PPSA99202] vrhello: done, exit 0
  ```

  What to read: `display live after … ms` (the vblank gate; a 5000 ms line
  means the link never counted), `RegisterBuffers2 (3 x 3968x2048) -> 0x0`, a
  `t=…s` line every second with `fps≈60`, `shown` climbing and close to
  `frames`, `missed=0`, `queue-full=0`, `err=0`, `step≈16.67 ms`, `worn=1`, and
  `PASS`. `pace[nap call ahead step]` are the predicted-time pacing numbers:
  `ahead` is how far the display time was when `BeginFrame` returned, `step`
  the gap between consecutive display times. `flip-rc` on the `run:` line is
  the VR port's own flip-status call: if it is non-zero the port does not report
  it (HLA does not use it either), `shown` stays 0 by design and the pass
  criterion falls back to the frame count and `err=0`; the picture in the
  headset is then the only proof.
- `run-vr-hello.sh` exit codes: 0 the `vrhello: done, exit` line appeared, 1 it
  did not in the window, 2 usage or a missing package, 3 no log stream, 4
  install/launch failed, 5 empty capture or the title would not stop. Exit 0
  means the title ended; **PASS is the title's own verdict**.
- If it fails, in order of likelihood:
  - `hmd2: … -> 0x8…` at a module load / `init failed at …`: the same failures
    as step 5, from a title now. `sceHmd2Open -> 0xffffffff` is the headset
    off/not worn.
  - `vrhello: FAIL vkCreateInstance` / `vkCreateDevice` / no device: RADV did
    not come up. Check the `direct pool` line of step 1 and the `[radv]` lines;
    a title with 0 heaps is step 1's failure, not this one.
  - `RegisterBuffers2 (3 x 3968x2048) -> 0x8…`: the interop question. The
    attribute is HLA's; try a smaller per-eye size (compile-time `kEyeWidth` /
    `kEyeHeight` in `title.cpp`) and rebuild — HLA's floor is 1024.
  - `display live after 5000 ms` then `queue-full` climbing and `fps` collapsing:
    the DP link never came up, or a flip went out before it did. Power-cycle the
    headset, make sure nothing else owns it, relaunch. Hmd2 staying wedged for
    the process is the known failure; the title still tears down cleanly.
  - `queue-full=0` but `shown=0` for the whole run with `flip-rc=0x00000000`:
    Hmd2 never consumed a frame. Check `worn=1` and that `param.json` in the
    package really carries attribute `0x62000000` / attribute3 `0x4080c40`
    (the verify script does). A non-zero `flip-rc` is different: the VR port
    does not report flip status, `shown=0` is expected, and the picture is the
    proof.
  - picture wrong: (a) **R and B swapped** → the linear descriptor's swizzle;
    (b) **blocky/diagonal mess** → the buffer is not the linear layout the
    descriptor describes (RADV wrote a tiled image); (c) **one eye lit** → the
    second descriptor at layer `+0x24`; (d) **squashed or shifted halves** →
    the per-eye viewport/source rect fields in the 0x148 layer; (e) **yellow bar
    stuck in the centre** → the pose pipeline (tracker), not the display.
  - `FAIL vkWaitForFences -> VK_TIMEOUT`/`DEVICE_LOST`: teardown logs
    `leaking the Vulkan image, buffers and device` and the title ends. That is
    correct: nothing is freed under a GPU that may still read it. Read the
    `[radv]` lines just above.
  - `teardown: done in … ms` but `done, exit` never appears: `LoadExec("exit")`
    was refused; `title.sh stop` ends a still-running title through the quit
    file (never a kill). If the title is stuck before its loop, that is the
    route; a forced kill is the last resort (step 6's warning).
- Leaving it: it exits itself after 30 s. `title.sh stop` (and therefore
  `run-vr-hello.sh`) writes `/data/ps5-splat-vr/title-quit`, which the title
  checks once a second and answers with a clean teardown and `done, exit 0`;
  it never kills. A title stuck before its loop cannot be stopped that way —
  close it from the console (PS → Options → Close App).

### 1d. Splats in the headset — the viewer title in VR mode — the goal

The same title as step 1b, now with a VR mode that is the **default**
(`MODE=vr` in `viewer.env`): the SHARP scene is rendered twice, once per eye,
by the same C6 renderer into Hmd2's eye buffers — 1984×2048 per eye side by
side, 60 rendered frames per second reprojected into the 120 Hz output. That is
step 1c's display path with a real scene in place of the pattern. The scene is
placed in front of the user once the headset is worn and the tracker's pose has
held for half a second, and stays world-locked there: the head moves through
it, the scene does not follow the head. Input is the DualSense **or** the two
PSVR2 Sense controllers, with one mapping on both (the one exception: a Sense
L1/R1 is a hold-vs-tap button, W26 - hold it to grab, tap it to step; the
DualSense's L1/R1 always step); **L1/R1 on the DualSense** and **a tap of a
Sense L1/R1** step through the scenes pushed
under `/data/ps5-splat-vr/scenes/` (sorted by folder name, wrapping) and a
switch keeps the world placement, scale and offset. **L2/R2** scale the scene as a **maquette** by default: shrinking glides its content centre onto a virtual table 0.7 m ahead and 0.35 m below the eyes, then L2/R2 grow and shrink it in place there (Settings → Comfort → **Scale** can pick Front or Centre instead; the viewer does not move), the **left stick walks** (forward/back, right/left), the **right stick
rises and turns** (up/down, right/left), **holding Cross for one second**
recentres, and **holding a Sense grip** moves and turns the scene with that hand
(both grips: scale in place between the hands). The background is black: the
clear colour and the comfort fade's backdrop are both black, and the TV mirror
shows the same buffers. With no input, or no controller at all, the run ends
itself after 30 minutes.

| Action | DualSense | Sense controllers |
| --- | --- | --- |
| Quit | **Options** | right: **Options** |
| Recentre (hold 1 s) | **Cross** | right: **Cross** |
| Previous scene | **L1** | left: **L1 tap** (a hold is the grab) |
| Next scene | **R1** | right: **R1 tap** (a hold is the grab) |
| Grab (hold): the scene follows that hand; both grips scale it about the point between them (W26) | — | left: **L1 hold**, right: **R1 hold** - a tap instead steps the scene |
| Grow / shrink the scene (default **maquette**: shrink puts it on a 0.7 m table ahead, then it scales there; Settings → Scale also offers Front and Centre) | **L2** shrink / **R2** grow (analogue, x2 per 3 s at full) | left: **L2** / right: **R2** (analogue) |
| Walk (**you** move: forward/back, right/left) | **left stick** (2.5 m/s default at full, quadratic; Settings) | left controller's **stick** |
| Rise / sink (**you** move: up/down) | **right stick** up/down (same move-speed curve) | right controller's **stick** up/down |
| Turn (**you** turn right/left) | **right stick** right/left (40 deg/s default at full, quadratic; Settings) | right controller's **stick** right/left |
| no action | Create, L3, R3, Square/Triangle/Circle | left: Create, Square/Triangle, L3; right: Circle, R3 |

A Sense grip press starts the grab at once, so the scene follows the hand from
the first frame. If it is released **within 250 ms** and the hand moved less
than **2 cm** and turned less than **3 deg**, it was a tap: the grab's own small
delta is undone and the press steps the scene like the DualSense's L1/R1 (left =
previous, right = next), logging `viewer: vr L1 tap -> scene N/M`. A longer
hold, or more motion, stays a grab. Both grips down is never a tap.


The buttons are mapped by meaning, not by position, and both devices feed the
same table (`platform/ps5/viewer/pad_input.h`): Options is the only quit button,
and it is on the DualSense and the right Sense controller. **R3 no longer
recentres and a short Cross press does nothing**: a 1 s hold is what recentres,
and the head-locked HUD under the view centre shows a thin bar filling up and
"Hold to recentre" while it is held. Releasing early cancels. Create is the
system's screenshot button on the DualSense and the left Sense controller's menu
key in other titles, but here it does **nothing** on either device, like the
DualSense's. Every device is optional and any of them can trigger an action; a
controller that is off is retried every 2 s and logged once, and the session
runs with whatever is there.

Build, verify and read the plan (all on the Mac; nothing sent):

```sh
platform/ps5/ggml/build.sh                     # only when core/ or a shader changed
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/viewer                    # build/ps5/title/PPSA99202/ (~27 MiB)
platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99202
platform/ps5/scripts/run-viewer.sh --dry-run   # every request, sends nothing
```

`verify-package.sh` must end `verify-package: PASS`. Its VR checks are:
`param.json` carries the PS VR2 identity `0x62000000` / `0x4080c40` (without it
Hmd2 never reports the headset worn and never consumes a frame — step 1c's
finding), the linked PIE needs VideoOut, Agc, AgcDriver, UserService, Pad and
Sysmodule, and the eboot carries the VR path's own strings.

Then, headset on and worn, nothing else using it:

```sh
platform/ps5/scripts/run-viewer.sh                    # VR, 30 min cap, first scene
platform/ps5/scripts/run-viewer.sh --seconds 600      # ... ends itself after 10 min
platform/ps5/scripts/run-viewer.sh --scene 02_living_lite
```

`run-viewer.sh` prints the scene folders it finds on the console (that is the
L1/R1 order), writes `MODE=vr` — and `SECONDS=` only when `--seconds` is given —
to `/data/ps5-splat-vr/bench/viewer.env`, installs the package when `title.sh
status` does not verify it, launches, captures
`$SPLAT_DATA_DIR/runs/console/<UTC>-viewer-title.log`, and stops a title that is
still running after the window. Exit codes as step 1b; **PASS is the title's own
verdict**. `--seconds` also shortens the capture window (`seconds + 90`);
without it the window is the 30-minute cap plus 90 s, and `title-capture.py`
keeps streaming until that window closes even after the end line, so pass
`--seconds` for a quick look.

- What the user sees: the first scene at the photo's own viewpoint, world scale
  1.0, filling both eyes, **head tracking live in all six degrees of freedom** —
  turn, nod **or roll** your head and the view follows while the scene stays
  put: rolling your head left (left ear down) turns the picture clockwise on
  your retina by the same angle, as a fixed world must. A scene that rolls *with*
  your head, or rolls the other way, is the bug the 2026-10-26 session found
  (`runs/vr4_pose/REPORT.md`); moving your head 5 cm shows the near parts of the
  photo sliding against the far ones (parallax). Past the photo's border the
  capture-frustum fade blends into **black**: the renderer's clear colour and
  its comfort backdrop are the same value, set to 0 for VR, so the void around
  the photo and the fade past the comfort radius are black in both eyes and on
  the TV mirror (the mirror shows the same eye buffers). The frames submitted
  before the scene is placed are the untouched (black) eye buffers.
- **First placement (VR7)**: the scene is placed when the headset is worn, the
  tracker has a pose, and that pose has **held for 0.5 s with no tracker jump**
  (more than 10 deg or 4 cm in one sample - a SLAM re-anchor, not a head turn).
  The 2026-10-27 00:36/00:43 sessions placed on the very first frame: the
  tracker's SLAM reset then moved the same head 94-98 deg in its new local
  frame in one 16.7 ms sample and the photo stayed where the old frame had put
  it, ~90 deg to the user's right until they recentred (`runs/vr7/REPORT.md`).
  The wait shows `vr waiting for the headset: worn=… tracked=… held=… s` once
  every 5 s, and `first placement: the tracker's pose jumped` when a jump
  restarts the window. The placement uses **the same call as the Cross hold**
  (`viewer: vr headset worn and tracked after … s (… waiting frames): the pose
  held … s, so the scene is placed at the head (… the recentre path, … deg of
  yaw removed …)`), so a first frame can never be placed differently from a
  recentre.
- `COMFORT_RADIUS=` is the head-travel distance at which the scene is fully
  **black** (it fades to the backdrop). The VR default is **2.0 m** (fade starts
  at 1 m); the desktop's 0.25 m also works in VR but fades from 12.5 cm, which
  is why the first headset run (`runs/console_session_8`) had the scene dim as
  soon as the user stepped. Do not put `COMFORT_RADIUS=0.25` in `viewer.env` for
  a standing session.
- **Scaling (VR9: the maquette)**: R2 grows, L2 shrinks. Settings → Comfort →
  **Scale** picks the mode:
  - **Maquette** (default): a table point **T** is frozen at the press's start,
    **0.7 m ahead** along the horizontal gaze and **0.35 m below eye height**.
    As the scale s goes from 1 down to **s_m** — the scale at which the scene's
    robust (5-95 percentile) box is **0.6 m wide** — the scene's content centre
    C (the median of the splats in the visible cone) glides from C0 to T with
    `C(s) = lerp(C0, T, smoothstep((1-s)/(1-s_m)))`, so it shrinks and lands on
    the table in one motion. **Below s_m**, and growing back from the table,
    L2/R2 scale **about T**, so the maquette visibly grows or shrinks in place;
    growing past s_m glides back the same way to the original placement at
    s = 1 (the round trip returns the exact offset). **Above 1** the scene also
    scales about T: the content centre recedes as `T + s(C0-T)` while the
    angular size still grows (~1.3x at 2x, ~1.5x at 4x for a 1.6 m centre), so
    R2 from full size still reads as "bigger". The readout is
    **`Maquette 0.08x`**; the vignette and the ring at T are unchanged. A press
    is rate-limited as before (x2 per 3 s at full, eased 0.5 s) with the
    content centre's travel capped at 1.5 m/s.
  - **Front** (VR8, for comparison): the **5th-percentile depth** of the splats
    in the head-forward cone, clamped to 0.3-30 m. The nearest surface keeps
    its place and size; everything behind expands away from it.
  - **Centre** (VR8): the **median depth** in the cone (clamped to 0.3-60 m).
    The middle stays; the near content moves, and growth stops before the
    nearest content reaches **0.5 m**.

  The cone measure comes from a decimated copy of the scene's splat positions
  (about 50k points, opacity > 0.3), taken once per loaded scene; a scene
  without it falls back to the VR7 1.5 m pivot (maquette then uses the 1.6 m
  scene point as C0 and never glides). The viewer never moves. While pressing:
  a small world-locked ring at the pivot/T, the head-locked readout, and a
  light peripheral vignette. The readout stays 1.5 s, the marker 0.5 s.
  `viewer: vr scale start: R2 from 1.00x about the maquette pivot …` and the
  `viewer: vr maquette: table … content centre … maquette scale …` line record
  T, C0, the robust extent and s_m.
- **Walking (VR7: the sticks move *you*)**: left stick forward walks into the
  scene (the scene comes towards you), left stick right steps right, right stick
  up rises, right stick right turns you to your right. Until VR6 the sticks
  moved the *scene* the other way. Settings → Comfort gives separate **Move
  speed** (0.75 / 1.5 / 2.5 / 4 m/s, default **2.5**) and **Turn speed**
  (20 / 40 / 60 / 90 deg/s, default **40**) rows; a quadratic stick curve means
  half a stick is a quarter of the speed. Dead zone 0.15, rescaled. Both
  velocities ease in and out with a **150 ms time constant** and are integrated
  with the measured frame dt; the input dt is capped at 20 ms, so a hitch no
  longer becomes a double-length step. Because the stick turn is a world
  rotation, Hmd2's 120 Hz reprojection cannot smooth it: it remains one step per
  rendered 60 Hz frame - **0.33 deg** at 20 deg/s, **0.67** at 40, **1.0** at
  60, **1.5** at 90 - each held for two 120 Hz refresh periods. The scene
  description below the photo moves and scales with the scene; it is drawn with
  the smooth Inter font at about one texel per headset pixel (VR9), not the old
  bitmap font.
- **Recentre**: hold Cross for one second (DualSense or the right Sense
  controller). The capture point goes back to where the head is, looking
  forward, the head's pitch and roll come out (the photo appears upright), and
  the scale, offset and turn are reset. A short press and R3 do nothing; letting
  go early cancels and the log says so.
- Scenes: L1/R1 switch (on the DualSense), or a **tap** of a Sense L1/R1 (a
  hold there is the grab). **A switch keeps the world placement, scale and offset**:
  the new scene's capture camera appears at the same world anchor as the
  previous one, so exploring is not interrupted by a recentre; only the new
  scene's centre and near-content depths change (the next press's pivot). The
  next scene is preloaded while the current one runs when it fits the spec's
  +64 MiB preload budget (1.18 M splats is ~34 MB of attributes), so the switch
  is a descriptor swap of a few ms; the
  headset keeps reprojecting the last submitted frame during any switch, never
  black. A scene over the budget is read on demand and the log says
  `preload skipped … over the +64 MiB budget`. **Every press lands**: the
  one-deep slot holds the *next* scene, so an L1 (going back) drops that
  preload and reads its own scene - the log says `loaded on demand (forward
  preload dropped for the press)` and `read … ms + stage … ms + commit … ms`.
  A switch that lands on the preload is unchanged (`preloaded, committed in …
  ms`). This is VR6's fix for "L1 sometimes did nothing": 22 of the 61 presses
  in the 2026-10-26 23:17 session were lost that way, every one an L1, because
  the on-demand upload was refused while the forward preload held the slot
  (`a scene is already staged`).
- The TV shows the headset's view: Hmd2's social screen (mirroring) is on by
  default, so a Create-button recording captures what the headset shows instead
  of black. It is Hmd2's compositor that writes it, next to the frames this loop
  already submits, so it costs the loop no frame, thread or copy; the log says
  `vr mirror: on (Hmd2 social screen …)` once, then `mirror=…fps` in the
  one-second line. `MIRROR=0` in `viewer.env` (or `--env` a file holding it)
  turns it off if it ever costs VR frames; `mirror=off` then replaces the rate.
- Success (klog; the kernel prefix is the process name, match the text):

  ```
  <118>[PPSA99202] viewer: config /data/ps5-splat-vr/bench/viewer.env: mode=vr scene=(first) seconds=1800 comfort_radius=0.250 m world_scale=1.00 preload=1 mirror=1
  <118>[PPSA99202] viewer: mode vr (MODE=vr in /data/ps5-splat-vr/bench/viewer.env); TV mode is MODE=tv
  <118>[PPSA99202] viewer: vr session (mode vr): eye 1984x2048 per eye, 60 rendered -> 120 Hz reprojected, cap 1800 s
  <118>[PPSA99202] viewer: vr log: /data/ps5-splat-vr/logs/session-20261027-190102.csv (one row per submitted frame, flushed every second, at most 32 MiB)
  <118>[PPSA99202] viewer: vr scenes: 2 folder(s) under /data/ps5-splat-vr/scenes
  <118>[PPSA99202] viewer: vr scene 1/2 01_sharp_photo1: 1179648 splats, load 221.0 ms
  <118>[PPSA99202] viewer: vr capture intrinsics fx 1664.10 fy 1664.10 cx 960.0 cy 720.0, image 1920x1440
  <118>[PPSA99202] viewer: vr scene centre 1.62 m ahead on the capture camera axis, near content 1.85 m (scale: the maquette lands the cone's median centre on a table point by default; Settings > Scale also keeps VR8's Front 5th-percentile and Centre median pivots; L2/R2 never move the viewer)
  <118>[PPSA99202] viewer: vr eye buffer 0: 3968x2048 RGBA8 at 0x… (32505856 bytes, memory type 2)
  <118>[PPSA99202] hmd2: user 1, eye 3968x2048 x3 buffers (external, linear RGBA8)
  <118>[PPSA99202] hmd2: mirror memory work 0x… (align 0x…), display 0x… (align 0x…)
  <118>[PPSA99202] hmd2: TV mirror on: option 0x00000000 enable 0x00000000
  <118>[PPSA99202] hmd2: RegisterBuffers2 (3 x 3968x2048) -> 0x00000000
  <118>[PPSA99202] hmd2: display live after 1100 ms (vblank rc 0x00000000 count 0 -> 3)
  <118>[PPSA99202] hmd2: ready (firmware table 1360, 21 exports)
  <118>[PPSA99202] viewer: vr headset: 1984x2048 per eye, 3 buffers, refresh 120 Hz output, fov tangents out 1.842 in 0.947 top 1.329 bottom 1.329 -> fx 711.37 fy 770.50 cx 992.0 cy 1024.0, per-eye principal point +318.3 / -318.3 px
  <118>[PPSA99202] viewer: vr mirror: on (Hmd2 social screen: the headset's view on the TV, so the console's recording captures it), display buffer … MiB [… = 1920x1080 at 4 bytes per pixel]
  <118>[PPSA99202] viewer: vr renderer: 1984x2048 stereo (side by side), shaders /app0/shaders, create … ms
  <118>[PPSA99202] viewer: vr warm-up frame: project … sort … draw … ms (both eyes), … visible splats
  <118>[PPSA99202] viewer: vr pad: DualSense controller open for user 1 (handle …)
  <118>[PPSA99202] viewer: vr pad: left Sense controller open for user 1 (handle …, raw buttons 0x00000000)
  <118>[PPSA99202] viewer: vr pad: right Sense controller open for user 1 (handle …, raw buttons 0x00000000)
  <118>[PPSA99202] viewer: vr input: Options opens the menu (DualSense or right Sense; its Quit item ends the session), hold Cross 1 s to recentre (R3 and a short press do nothing), L1/R1 change scene (a Sense grip hold grabs instead; a tap steps), L2/R2 scale the scene about a point ahead of the eyes, left stick walks (forward/back, right/left), right stick rises and turns right; Create does nothing, one table for all pads
  <118>[PPSA99202] hmd2: Hmd2 live at frame 1 (display +12000 us)
  <118>[PPSA99202] viewer: vr waiting for the headset: worn=0 tracked=0 after 0 s (up to 300 s)
  <118>[PPSA99202] viewer: vr waiting for the headset: worn=1 tracked=1 held=0.3 s after 1.0 s (up to 300 s; 0.5 s of a held pose places the scene)
  <118>[PPSA99202] viewer: vr headset worn and tracked after 2.1 s (63 waiting frames): the pose held 0.5 s, so the scene is placed at the head (capture camera at the eyes, looking forward - the recentre path, 0.8 deg of yaw removed, world scale 1.00), ipd 63.0 mm from the tracker's eye poses
  <118>[PPSA99202] viewer: vr first frame in … ms from the session start (… visible splats, eye buffer 0, … ms of work)
  <118>[PPSA99202] viewer: vr t=1s fps=60.0 project=0.28 sort=0.39 draw=10.62 (per eye 5.31) copy=0.21 gpu=11.29 ms visible=… fade=1.000 scale=1.00x ipd=63.0 mm slow=0/61 missed=0 shown=61 repeated=0 queue-full=0 err=0 worn=1
  <118>[PPSA99202] viewer: vr t=1s fps=60.0 project=0.28 sort=0.39 draw=10.62 (per eye 5.31) copy=0.21 gpu=11.29 ms visible=… fade=1.000 ipd=63.0 mm slow=0/61 missed=0 shown=61 repeated=0 queue-full=0 err=0 worn=1 mirror=60.0fps
  <118>[PPSA99202] viewer: vr mirror: 60.0 fps achieved over the first 1.0 s (Hmd2's compositor, next to the frames this loop already submits)
  <118>[PPSA99202] viewer: vr pose pos 35.0 162.0 -80.0 cm yaw 0.0 pitch 0.0 roll 0.0 deg frame=61
  …
  <118>[PPSA99202] viewer: vr Options pressed; ending
  <118>[PPSA99202] viewer: vr totals: … frames in … s (60.0 fps), GPU … ms/frame (project … sort … draw …, per eye …), copy … ms/frame, slow …/…, submitted=… shown=… missed=… repeated=… queue-full=… err=…
  <118>[PPSA99202] hmd2: stopped after … frames (unregister 0x00000000 close 0x00000000 disable VR mode 0x00000000)
  <118>[PPSA99202] viewer: vr teardown … ms
  <118>[PPSA99202] viewer: vr PASS … frames, 60.0 fps, … submitted, shown …, missed …, worst frame … ms
  <118>[PPSA99202] viewer: vr done, exit 0
  <118>[PPSA99202] viewer: done, exit 0
  ```

  What to read: `mode vr`; the `fov tangents` line, whose **per-eye principal
  point must be one negative and one positive number** (the headset's
  asymmetric frustum — two zeros mean the FOV call failed and the eyes will look
  squeezed); three `eye buffer` addresses 256-byte aligned; `headset worn and
  tracked after … s`, the moment the scene is placed; `Hmd2 live`; the three
  `vr pad:` lines (a device that is off prints `no … controller` once instead),
  each pad's state logged once per change (`connected` / `gone`, never at
  startup before `open`); and then one
  `t=…s` line per second: `fps≈60`, `project≈0.3 sort≈0.4 draw≈10–11 ms` (draw
  is both eyes, `per eye` is half of it), `gpu` under 16.6 ms, `shown` climbing,
  `missed=0` (a single 1 is the first frame), `queue-full=0`, `err=0`,
  `worn=1`, and `scale=` the scene's current scale (1.00x until the user uses
  L2/R2). `slow=n/m` counts frames whose work passed 16.6 ms; a `vr pose` line
  `worn=1`, `mirror=≈60fps` (the TV's rate; `off` when `MIRROR=0`, `failed`
  when Hmd2 refused it). `slow=n/m` counts frames whose work passed 16.6 ms; a `vr pose` line
  follows each one with the raw Hmd2 head position in cm and the scene-frame
  yaw, pitch and roll in degrees (the one-second grid a recorded video can be
  matched against; below). The run ends with `vr done, exit 0` and `run-viewer.sh`
  exits 0.

#### The head-pose and input log (matching a recorded video)

Each VR run writes **one CSV per run**,
`/data/ps5-splat-vr/logs/session-<YYYYmmdd-HHMMSS>.csv` (UTC, the folder is
created on the first run; a `/data` that refuses the write logs `cannot write …
the session runs without the pose log` and nothing else changes). It is one row
per submitted frame, flushed every second (a killed run keeps its rows up to the
last flush) and capped at 32 MiB — a 30-minute session at 60 fps is about 20 MiB,
so the cap only matters past the cap. `frame` is the submitted-frame counter, the
same one the `vr pose` line and the `t=…s` lines count, and `t_s` is seconds
since the session start (so `t_s` and the `vr pose` lines share one clock).

| Column | Meaning |
| --- | --- |
| `t_s` | seconds since the session started; rows begin with the first rendered frame (the wait for the headset submits no image, so it has no row) |
| `wall_utc` | wall clock, UTC, milliseconds: `2026-10-27T19:01:02.345Z` — the column to line a video's timecode up with |
| `frame` | submitted frame number, from 1 (60 per second) |
| `tracked` | 1 when the tracker had a pose for this frame; 0 means the values are the last good pose, held (the renderer holds its pose too) |
| `pos_x_m, pos_y_m, pos_z_m` | the raw Hmd2 device pose position, metres, tracker space: **+x right, +y up, forward -z** |
| `quat_x, quat_y, quat_z, quat_w` | the raw device orientation, x,y,z,w, same convention |
| `yaw_deg, pitch_deg, roll_deg` | the angles the renderer used, in the **scene** frame (after the placement offsets): yaw positive to the right, pitch positive looking down, roll positive with the right ear down |
| `dualsense_buttons`, `sense_left_buttons`, `sense_right_buttons` | each device's button word as read (`0x…`, the PS5 bitmask: Cross 0x4000, Options 0x8, L1 0x400, R1 0x800 — `platform/ps5/viewer/pad_input.h`) |
| `actions` | the action bits this frame (`0x…`: 1 the menu/panel button, 2 tooltips, 4 recentre — set on the frame the Cross hold completes, 8 previous scene, 16 next scene; `platform/ps5/viewer/pad_input.h`). The 2026-10-27 evidence file's old "1 quit, 2 recentre, 4 previous, 8 next" row predated the panel bit |
| `scale` | the scene's scale that frame (0.25–4.0; 1.0 is the capture size). VR9's default **maquette** glides the scene's content centre onto a table point T as it shrinks to s_m, then scales about T (Settings → Scale also offers VR8's Front and Centre pivots) |
| `offset_x_m, offset_y_m, offset_z_m` | the scene's translation, metres in the placement frame (`offset_y_m` is **down** positive): the sticks walk/rise you, and a scale press adds its pivot compensation here - `offset' = pivot + f * (offset - pivot)`. A Front press whose pivot is 0.8 m ahead moves the offset 0.8 m for a 2x growth |
| `turn_deg` | the accumulated turn about the user's vertical axis, degrees (positive turns the scene to the user's right): the right stick's and any the Sense grab added; the `yaw_deg` column already includes it. At the 40 deg/s default one 60 Hz rendered frame is 0.67 deg (20/60/90 deg/s: 0.33/1.0/1.5); reprojection is head-only, so each step is held for two 120 Hz refreshes |
| `hold_progress` | the recentre hold's progress, 0.00–1.00 (0 when Cross is not held; the bar on the overlay shows the same value) |
| `scene` | the scene folder on screen at that frame (quoted; a comma inside is safe) |
| `event` | what happened before this frame: `recentre`, `scene`, `scale start`, `scale end`, `move start`, `move end`, `grab start`, `grab end`, `quit`, `cap`, `fail`, or empty |

A scale press shows in `scale` and in `offset_*`: the offset absorbs the pivot
compensation so that the pivot point keeps its place in the picture while the
scene grows around it, and `pos_*` (the raw Hmd2 head pose) does not move. Read
it as "the scene moved", not "the user moved": the viewer's world position is
the head, and `pos_*`/`yaw_deg` stay continuous through a press. (VR6's press
left the offset alone - a pure zoom, the picture unchanged; VR5's moved the
camera by up to 139 m per press, the reported "flying".) A scene press that lands carries action bit 4 (L1)
or 8 (R1) on the press frame and the `scene` column changes on that frame; a
press whose folder does not change is a lost press - before VR6 every L1 that
went back failed while the next scene was preloaded, and only the klog's
`FAIL stage … a scene is already staged` said so.

The Sense grab (W26) writes `grab start` / `grab end` in `event` and shows up in
`scale`, `offset_*` and `turn_deg` like any other scene change: there is one transform,
whatever moved it. A one-hand grab follows the hand (offset and turn); a two-hand grab
sets `scale` and the offset's pivot compensation, with the point between the hands fixed
in the picture. A quick still tap is a grab whose tiny delta is rolled back and whose
release writes `scene` (with action bit 4 or 8), so the row's transform is the one before
the press. `pos_*` and the comfort fade stay continuous through either, because the
viewer never moves — only the scene does. The hand's own pitch and roll are not
representable in the viewer's transform (a yaw `turn_deg` is all it stores), so only the
heading of the hand's rotation is applied; recorded in `docs/BACKLOG.md`.

Read it back (the console's file API; `ps5-env.sh`'s client is the same route,
`convert-inbox.sh` fetches `viewer.env` this way):

```sh
set -a; . ./.env.local; set +a
curl -s "http://$PS5_HOST:7070/api/ftp/list?path=%2Fdata%2Fps5-splat-vr%2Flogs"   # names + sizes
curl -s "http://$PS5_HOST:7070/api/ftp/text?path=%2Fdata%2Fps5-splat-vr%2Flogs%2Fsession-20261027-190102.csv" \
  | python3 -c 'import json,sys; sys.stdout.write(json.load(sys.stdin)["text"])' > session.csv
head -2 session.csv
```

Matching a video: read `wall_utc` on the row where the user's action is visible
(a scene switch is a visible cut, a recentre is a jump) and subtract it from the
video's own timestamp; from then on one row is one rendered frame and the
`yaw/pitch/roll` columns say what the head was doing. The per-second `vr pose`
lines in klog are the same values on a one-second grid, so a klog that was
captured with the video is enough to place the CSV's timeline in it.

- If it fails, in order of likelihood:
  - `vr waiting for the headset: worn=0 tracked=0 …` for five minutes, then
    `FAIL no usable pose after 300 s`: the headset is not being worn, or the
    tracker has no pose (play-area setup, controllers not tracked). The title
    ends cleanly (exit 1) with VR mode off. Put the headset on and relaunch; the
    wait is bounded on purpose and logs every 5 s.
  - `FAIL hmd2 init at …`: the VR module/VR mode failures of step 1c, unchanged.
    `sceHmd2Open -> 0xffffffff` is again "headset off".
  - A picture but wrong eyes (squeezed, or the same view in both): check the
    `per-eye principal point` line, then the three `eye buffer` addresses.
  - The picture looks washed out or too dark: the eye buffer's transfer
    function. The renderer writes display-ready sRGB-encoded bytes through an
    sRGB target (the TV path's convention); Hmd2 samples them as UNORM8. If the
    headset's compositor expected linear values instead, every colour would be
    off by the sRGB curve — compare one eye against a desktop `--screenshot`.
  - `fps` far below 60 with `draw` in the tens of ms: the GPU cannot afford
    1984×2048 per eye for this scene. `draw` (and its `per eye` half) is the
    number to quote; try `--scene` with fewer splats.
  - `FAIL eye buffer copy: vkWaitForFences -> VK_TIMEOUT`: the GPU did not
    finish a copy within 500 ms. Teardown then logs `leaking …`, the headset is
    still taken out of VR mode, and the title ends; read the `[radv]` lines.
  - `vr preload skipped … over the +64 MiB budget`: not a failure. The scene
    loads on demand and the switch costs ~250 ms with the last frame held.
  - `viewer: vr pad: no left Sense controller … retrying every 2 s` (or right,
    or the DualSense): not a failure. That controller is simply off or not
    paired; the others, or the run's own length, still work, and the line is
    printed once per device. Plug it in and it is picked up within 2 s.
  - `viewer: vr pad: left Sense raw buttons 0x…` (and `right`): that Sense
    controller's button word, printed when it changes (at most 5 lines a second,
    plus its value on the `open` line). Press Cross on the right controller and
    it must read `0x00004000`. A word that never changes from `0x00000000` while
    the controller is `connected` means libScePad is not delivering Sense
    buttons to a pad that was never registered with
    `sceVrTracker2RegisterDevice`; Options would then not quit either.
- Leaving it: Options on the DualSense or the right Sense controller quits (clean
  teardown, home screen), `title.sh stop` writes the quit file and the VR loop
  answers with the same teardown, and the 30-minute cap ends it by itself. The
  headset is left out of VR mode on every path.
- `MODE=tv` in this package still runs step 1b's mono path, but the identity in
  `param.json` is the VR one now (`0x62000000` / `0x4080c40`, VR-only bit set),
  so TV mode under it is unproven — recorded in `docs/BACKLOG.md`.

### 1e. Scripted UI runs (TV)

`ui-test.sh` saves `viewer.env`, writes `MODE=tv UI=1 SCRIPT=<remote> SECONDS=<N+30>`,
uploads the script, launches the viewer, waits (klog) for `viewer: script done`, pulls
`shots/*.png` read-only via `GET /api/ftp/download?path=` to
`$SPLAT_DATA_DIR/runs/ui_v1/shots/<UTC>-<script>/`, and restores `viewer.env` (sha256
checked). N is `--seconds` or the script's last event; `--dry-run` prints every request
and sends nothing.

```sh
platform/ps5/scripts/ui-test.sh platform/ps5/scripts/ui-tests/browse.txt --dry-run
platform/ps5/scripts/ui-test.sh platform/ps5/scripts/ui-tests/browse.txt
```

Keys: `UI=1` (boot into the panel), `SCRIPT=<path>` (`<t> press|down|up|hold|
stick|shot|mark|quit` lines), `SHOTS_DIR` (default `/data/ps5-splat-vr/shots`, created
when missing; a relative value is resolved under `/data/ps5-splat-vr`, never `/`). klog:
`viewer: script shot <name> <bytes> bytes`, `viewer: script done`; shots are the TV
frame (BGRA readback written as an RGBA PNG). Exit: 0 done, 1 no `script done`, 2
usage, 3 no log stream, 4 put/install/launch, 5 no stop, 6 no restore.

### 1f. Morning check in the headset (the panel) — 2 min, writes nothing

The VR session now carries the same panel as the TV (`UiHost`, W16). `MODE=vr` is the
default; with no `UI` key in `viewer.env` the panel is available and the first scene loads
as before, `UI=1` boots into the panel, `UI=0` turns it off. Install and launch with
`platform/ps5/scripts/run-viewer.sh`, then, headset on:

- [ ] 1. Options (DualSense or right Sense) opens the panel in front of you; klog: `viewer: ui open`.
- [ ] 2. **No skew**: turn your head left/right up to ~20 deg and pitch up/down — the panel stays a square, upright rectangle (it may slide toward the edge); past ~17.5 deg it eases back in front over ~0.3 s instead of becoming a trapezoid.
- [ ] 3. Focus moves: d-pad and left stick move between tiles, Cross opens, Circle goes back.
- [ ] 4. L1/R1 page or change tab and the highlighted row stays in view.
- [ ] 5. Folder → scene → Cross: the scene loads, the panel closes (`viewer: ui command view_scene`, `viewer: ui close`).
- [ ] 6. L1/R1 now step through that album's scenes, not the boot folder's (klog: `viewer: vr scene N/M … (album …)`).
- [ ] 7. Options again: the panel comes back over the scene, and the scene is still rendering behind it.
- [ ] 8. While it is up, sticks and L2/R2 change nothing in the scene; Circle at the folders (the panel's top) returns to the scene and resumes them. Options inside the panel opens Settings.
- [ ] 9. With the panel closed, hold Cross 1 s: the scene re-anchors in front of you and the scale goes back to 1.0x.
- [ ] 10. Settings now has **Comfort** (Scale: Maquette/Front/Centre, Move speed, Turn speed) and **Display** (Edge softness, Blending) above the System rows. Cycle each with Left/Right or Cross; reopen Settings and the values are still there (ui.json). Options → Settings (scroll to the end) → Quit ends the title cleanly.
- [ ] 11. **Scaling, the maquette** (the default): on a scene at full size, hold **L2**. The scene shrinks and visibly **lands on a virtual table** in front of you (0.7 m ahead, 0.35 m below eye level) instead of rushing past you; the klog prints `viewer: vr scale start: L2 … about the maquette pivot` and `viewer: vr maquette: table … maquette scale 0.NNNx`. Release, then hold **R2**: the maquette grows **in place on the table**, then past the landing scale it glides back to the full-size placement. Release and hold R2 again from full size: it must read as "bigger" (the content centre recedes a little but the angular size grows); the readout says `Maquette N.NNx`. **Recentre** (hold Cross) returns scale to 1.00x and the original placement.
- [ ] 12. **Scale for comparison**: Settings → Scale → Front, repeat L2/R2 — the foreground stays put and the background expands (VR8); then Centre — the middle stays and the near content moves, with growth stopping before content reaches ~0.5 m. The ring is at the pivot/T in every mode, and the viewer never moves.
- [ ] 13. Speeds/smoothing: with Move speed 2.5 and Turn speed 40, a full right-stick push turns smoothly (no once-a-second double step); try 20 and 90 to hear/feel the difference. A quick stick tap should ease in and out, not snap.
- [ ] 14. Display A/B: Edge softness Standard vs Soft changes the splat edges; Blending Linear vs sRGB changes the mid-tone brightness (sRGB/gamma is a little darker/flatter, like Spark). Toggling must not crash or lose the scene.
- [ ] 15. Right Sense laser: a 12 px white ring follows where the controller points on the panel; klog says `viewer: pointer hit x y` at most once a second.
- [ ] 16. R2 clicks the widget under the ring (same as Cross), and the d-pad/left stick still move focus with the ring shown or gone.
- [ ] 17. Point away from the panel or hide the controller: the ring disappears, `pointer hit` stops, and navigation keeps working (no pointer errors in klog).
- [ ] 18. Grab, one hand (W26): hold the left controller's **L1** (or the right's **R1**) and move that hand — the scene follows it, in translation and in turn. klog: `viewer: grab start: left hand, scale=1.00x`, and on release `viewer: grab end: scale=…`. Taking hold and letting go must not move the picture by themselves. A quick still **tap** (under 250 ms, hand not moved 2 cm) must not move the picture either: it steps the album instead (`viewer: vr L1 tap -> scene N/M`).
- [ ] 19. Grab, two hands: hold both grips and move your hands apart/together — the scene scales about the point between them, that point keeps its place, and **you** do not move. The scale stops at 0.25x / 4.0x, never "flies".
- [ ] 20. With a grip held, the sticks still walk/turn you and L2/R2 still scale on top of the grab; a grip held while Options opens ends the gesture (`viewer: grab end: … (the menu suspended the scene controls)`) and the scene keeps its place.
- [ ] 21. Sense L1/R1 are both keys: a **tap** (released within 250 ms with the hand still) steps the scene like the DualSense's L1/R1 — left L1 = previous, right R1 = next — and a **hold** (or more than 2 cm / 3 deg of motion) is the grab. The panel's ViewScene still works too.
- [ ] 22. **Flicker (VR9)**: with the panel open, look at a row's text and at the scene label under the photo from ~1 m and from arm's length. Both must be **steady** — no crawling/dancing glyph edges as the head moves (the overlay textures now carry a full mip chain, trilinear + anisotropic where RADV supports it). Step back through the maquette scales (0.25x) and check the label and panel stay smooth, not blocky.
- [ ] 23. **Keyboard (VR9)**: Scenes → New folder. The grid is **QWERTY** (1-0, qwertyuiop, asdfghjkl + Shift, zxcvbnm, then . - _ Space Delete OK), not ABC. **Hold** Down/Right (d-pad or left stick): the focus moves once, waits ~0.35 s, then repeats and visibly speeds up (8 to 20 steps/s) — no need to tap repeatedly. Shift types one capital, Square deletes, OK creates the folder.
- [ ] 24. **System keyboard hook**: in this build the PS5 IME is not wired (the screen asks a text-input service that reports "not available"), so the panel grid must appear every time; there is no dead screen and no console error. The console IME is a separate spike.

`viewer: grab` does not appear at all: look for the two registration lines
`viewer: grab: left Sense pad … registered with VrTracker2 (device 2)` (and `right`,
device 1). A failed registration means `controller_pose` has no pose and the tracked
grab stays off — sticks, triggers, Cross hold and the panel are unaffected, and so is
the scene. `viewer: vr pad: … raw buttons 0x…` shows the grip press itself (`0x400` on
the left pad, `0x800` on the right).

### 2. probe_agc — ~15 s — log `<UTC>-probe_agc.log`

```sh
PS5_RUN_READY='vkhello-agc' platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_agc.elf 6
```

- Success: `vkhello-agc: imports resolved 8/8`, `sceAgcInit(8) -> 0x00000000`, `vkhello-agc: PASS`.
- If it fails, **continue** (the title gate already proved AGC links in a title; this is the payload's control): no start line (exit 1) = the loader cannot resolve the AGC imports; `imports resolved 7/8` names the module that did not map; a non-zero `sceAgcInit` means AGC refuses a payload process.

### 3. probe_mem — ~15 s — log `<UTC>-probe_mem.log`

```sh
PS5_RUN_READY='vkhello-probe' platform/ps5/scripts/run.sh platform/ps5/vk_hello/probe_mem.elf 8
```

- Expected now: `direct memory size 0 bytes`, `available direct memory rv=0x8002000c`, flexible ~8.00 GiB — the payload's budget, and the reason step 1 exists. That is a measurement, not a failure.
- If it fails to start, **continue**. No start line with step 2 green means only this ELF failed to load (it links no AGC).

### 4. vk_hello as a payload — optional control, 1-2 min, 97 MiB — log `<UTC>-vk_hello.log`

```sh
platform/ps5/vk_hello/run-on-console.sh 30
```

- Expected: `memory: 0 heaps, 0 types` and `FAIL no host-visible memory type` (session-1 result: no direct pool, no heap). This is the negative control for step 1 — keep the two logs side by side.
- If it instead reports `1 heaps` and `PASS`, the payload route works on this console too; record it and re-read `spike-b-memory.md` §1 before changing the plan.

### 5. vr_hello — ~45 s, headset on and worn — log `<UTC>-vr_hello.log`

```sh
platform/ps5/scripts/run.sh platform/ps5/vr_hello/vr_hello.elf 40
```

- Success: `hmd2: ready (firmware table 1360, 21 exports)`, a `vrhello: t=…s frames=… tracked=1 err=0` line every second, `vrhello: done, 1800 frames in 30 s`. Eyes: left dark green, right dark blue, grid + crosshair + white bar that moves when you turn your head.
- If it fails, **continue** (the benches do not need VR). `hmd2: … -> 0x8…` at a module load = a payload may not load system `.sprx`; `sceHmd2Open -> 0xffffffff` = headset off/not worn; `RegisterBuffers2 … -> 0x8…` is the interop question (retry 2048, then 4096 per eye); a wrong picture is swizzle/tiling/layer, not a crash.
- VR is still a payload in this step (its source is the payload `main.cpp`): it
  needs the title route for the same direct-memory reason. The title version is
  step 1c — same pattern, but RADV fills HLA's linear RGBA8 eye buffers; this
  payload run is the CPU-filled control.

### 6. push-data — ~3 min at 10 MB/s, up to ~20 min slower — log: `tee` it

```sh
platform/ps5/scripts/push-data.sh --dry-run     # requests only, no console I/O
platform/ps5/scripts/push-data.sh               # 1.27 GiB, resumable
platform/ps5/scripts/push-data.sh --verify      # sizes only
```

- Success: `…: complete and verified, … bytes` per file, ending `push-data: done.`; then three `OK` rows and `push-data: verify: PASS (all sizes match)`, exit 0.
- If it fails, **stop** (the benches need the data). Exit 3 no ProsperoMgr on 7070; exit 1 an upload failed — re-run the plain command, it resumes from the server checkpoint; exit 2 a local file is missing or a path was rejected; `DIFF` in `--verify` = push again. `--dry-run` prints the console address: do not paste it into the repo.
- No log file of its own: `… 2>&1 | tee "$SPLAT_DATA_DIR/runs/console/push-data-$(date -u +%Y%m%dT%H%M%SZ).txt"`.
- Scenes for the viewer title (step 1b) go through the same script:
  `push-data.sh --scene DIR` (one folder) or `--scenes DIR` (every immediate
  subfolder with `scene.psplat`), which plans only those files and writes
  `/data/ps5-splat-vr/scenes/<name>/{scene.psplat,meta.json}` (and creates
  `/data/ps5-splat-vr/bench/` for `viewer.env`). Re-running it skips files whose
  size already matches.
- `push-data.sh` writes `bench.env` only when it does not exist, and step 7 **replaces** it on the console with `BENCH=<name>` before each run. Keep driver settings in a local file and pass it with `--env LOCAL_FILE` (they are appended after `BENCH=`), otherwise a hand-edited remote `bench.env` is overwritten.

### 7. The GPU benches — the one bench title

`op_audit`, `da2_bench` and `sharp_bench` link the same RADV archive as
`vk_hello`; in a payload RADV gets **no direct pool and no heap**, so their GPU
runs are meaningless there (the payload route is only the negative control:
`memory: 0 heaps`). They run as the one homebrew title, `PPSA99202` — the same
tile as the viewer and vr_hello — whose program reads `BENCH=` from
`/data/ps5-splat-vr/bench/bench.env` and calls that bench: one 68.7 MiB package
for all three, one install, one tile. Why one title and not three, and what a
title changes for a bench: `docs/research/spike-a.md` §2.4.

Build it on the Mac (a title package, not a payload ELF), then read the plan
before sending anything:

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/bench bench-title     # build/ps5/title/PPSA99202/ (68.7 MiB)
platform/ps5/bench/run-bench.sh --title --dry-run op_audit
```

The dry run prints, in order: the `bench.env` body (`BENCH=op_audit`), the
`title.sh put` request, the `status`/`install` requests (install is skipped when
`status` already verifies the package), the `launch` request, the klog target
and end pattern, and the `stop` request. Nothing is sent.

Then one command per bench (klog is started by the script, so it is connected
before the launch):

```sh
platform/ps5/bench/run-bench.sh --title op_audit       # 1200 s window
platform/ps5/bench/run-bench.sh --title da2_bench      # 300 s
platform/ps5/bench/run-bench.sh --title sharp_bench    # 900 s
```

- Each run: rewrites `bench.env` (`BENCH=<name>`; add driver settings with
  `--env LOCAL_FILE`, because the remote file is replaced), installs the
  package if `title.sh status` does not already pass, launches it, captures
  `$SPLAT_DATA_DIR/runs/console/<UTC>-<name>-title.log`, and stops the title
  once that bench's `done, exit N` line appears (`title.sh stop`; after a
  normal end the app has already ended itself, so it prints "not running").
- Success is the bench's own `PASS` line in the capture, not the command's exit
  code (0 = the end line appeared, 1 = it did not within the window, 2 usage or
  missing package, 3 no log stream, 4 put/install/launch failed, 5 empty capture
  or the title would not stop).
- First build/install uploads 68.7 MiB; `title.sh` sets `PS5_HTTP_TIMEOUT=300` for
  it. After that every bench run is a ~74-byte selector plus a launch.
- The start of every capture is the dispatcher's lines and the pool lines
  (`bench_title: start …`, `bench_title: driver stderr capture …`,
  `bench_title: bench <name> (from …)`, `<name>: direct pool …`,
  `<name>: flexible pool …`). If `direct pool` is 0 B the process did not get
  the app pool: record it, that is the one result that sinks the title route
  (step 1 is the control).

What each bench's `PASS` says (unchanged text from `spike-a.md` §7):

- `op_audit`: a device line ending `matrix cores: coopmat1` (`coopmat2`/`none` is the answer, not a fault), `pass driver-default: ok=35 fail=0 unsupported=0`, `summary ok=70 fail=0 unsupported=0`, `op_audit: PASS`. 70 = both coopmat passes; a single pass gives `ok=35` and says why.
- `da2_bench`: `inference … s, … submits, 499896 splats`, `wrote /data/ps5-splat-vr/bench/photo.psplat (13.35 MiB)`, `da2_bench: PASS`.
- `sharp_bench`: `timings … total … s` with total ≤ 60 s (the spec gate), `wrote … photo.sharp.psplat (31.50 MiB)`, `sharp_bench: PASS`. Desktop reference: 49.6 s, 2.49 GiB peak RSS.

The payload route is kept as the control and changes nothing else:

```sh
platform/ps5/bench/run-bench.sh --payload op_audit 1200   # 68 MiB, expect memory: 0 heaps
```

`run-bench.sh <name> [seconds]` (no flag) is still that payload route.
`platform/ps5/scripts/run.sh … .stripped.elf` with `PS5_RUN_READY='op_audit:'`
also still works; the title route above is what the console run uses.

### 7b. Convert photos into scenes on the console

The same one title also converts: `BENCH=convert` selects
`platform/ps5/bench/convert_inbox.cpp`, which turns every photo in
`/data/ps5-splat-vr/inbox/*.jpg|*.jpeg|*.png` (sorted by name) that has no
`/data/ps5-splat-vr/scenes/<stem>/` yet into a scene folder, on the console GPU.
It is the same SHARP backend, the same EXIF-or-30-mm focal length and the same
`write_scene_folder` the desktop job runner uses; the model is loaded once for
the batch and unloaded at the end. Nothing in the inbox is deleted, moved or
renamed, and an existing scene folder is never overwritten, so a re-run converts
only what is new. A photo that fails is logged and skipped, and the batch goes on.

One command does the whole round trip — build the convert mode, upload the given
photos, run it to completion with klog captured, then rebuild and reinstall the
viewer and restore `viewer.env`:

```sh
platform/ps5/scripts/convert-inbox.sh --dry-run                 # every step, nothing sent
platform/ps5/scripts/convert-inbox.sh                            # convert what is in the inbox
platform/ps5/scripts/convert-inbox.sh "$SPLAT_DATA_DIR/golden/immichvr/4.jpg"
```

- It reads `/data/ps5-splat-vr/bench/viewer.env` first (copy + sha256), writes
  `BENCH=convert` to `bench.env`, installs the convert build into `PPSA99202`,
  captures `$SPLAT_DATA_DIR/runs/console/<UTC>-convert-title.log`, stops the
  title, then runs `make -C platform/ps5/viewer` and installs the viewer again.
  At the end `viewer.env` is read back and its sha256 compared; a changed file is
  put back from the saved copy.
- The photo name becomes the scene folder name (`immichvr-4.jpg` →
  `scenes/immichvr-4/`), so pass names that are `[A-Za-z0-9._-]+` and lowercase
  `.jpg`/`.jpeg`/`.png`. A photo already in the inbox with the same size is not
  uploaded again; a different size is refused rather than overwriting it.
- Per photo the capture has one line — name, seconds, splats, result:

  ```
  <118>[PPSA99202] convert: photo immichvr-4.jpg: 14.31 s, 1179648 splats, ok: wrote /data/ps5-splat-vr/scenes/immichvr-4, inference 11.91 s, 159 submits, focal 1664.1 px
  <118>[PPSA99202] convert: skip immichvr-3.jpg (scene folder /data/ps5-splat-vr/scenes/immichvr-3 exists)
  <118>[PPSA99202] convert: 2 converted, 0 skipped, 0 failed in 28.2 s
  <118>[PPSA99202] convert: PASS
  <118>[PPSA99202] convert: done, exit 0
  ```

- `convert: PASS` is the app's own verdict: every pending photo has a scene
  folder. The viewer lists them immediately (step 1b/1d, `--scene <stem>`).
- Failure lines to read: `FAIL setup` (no inbox directory, no model, no Vulkan
  device), `FAILED: cannot decode the photo` (a broken upload), and
  `FAIL n photo(s)` with the batch's exit 1. `WATCHDOG abandoning` means a GPU
  wait passed its bound and the process leaked the GPU state on purpose (step 7's
  policy); the log says which photo was in flight.
- Building needs `PS5_PAYLOAD_SDK` and `LLVM_CONFIG` as above; the script
  defaults the SDK to `$SPLAT_DATA_DIR/ref/radv-ps5/sdk` and finds the Homebrew
  LLVM when neither is exported.

### 8. After the benches — record what the console answered

- The three captures in `$SPLAT_DATA_DIR/runs/console/` are the evidence; nothing has to be cleaned up (no tool here deletes anything, and the package/selector stay installed for the next session).
- Record in `docs/research/op-audit.md` / `spike-a.md`: the `matrix cores:` value per pass and the coopmat side-by-side rows (`op_audit`), the SHARP `total` against the 60 s gate and its peak RSS, and `heap layout`/`direct pool` for every bench (does the title really get one ~11.44 GiB heap?).
- If a bench failed, the failure table in `spike-a.md` §7.2 and the `[radv]` lines at the end of the capture are where the answer is; `title.sh status` says whether the package is still installed and whether a title is running.
- Then reply **"done"**.

## Logs, and the end

- The capture is flushed as it goes, so Ctrl-C keeps everything: safe once the payload's `PASS`/`done` line appears, but the command then exits 130, not 0. `run.sh` always burns its whole window even if the payload needed seconds.
- Title runs: klog only. `run-bench.sh --title` starts the stream and connects it **before** `title.sh launch` (klogsrv replays nothing), reads until the bench's `done, exit` line, and writes the capture to `$SPLAT_DATA_DIR/runs/console/<UTC>-<name>-title.log`; a manual `klog.sh` must be connected before a manual launch and stopped by hand.
- Captures are noisy (`SceShellCore`): `grep -E 'bench_title:|op_audit:|da2_bench:|sharp_bench:|hmd2:|vrhello:|vk_hello:|vkhello:|viewer:' <log>`.
- After a console reboot: kstuff/HEN/ShadowMountPlus/ProsperoMgr are gone, so `title.sh launch` fails until they are back; the registered tiles and the files under `/data/homebrew` stay. `make -C platform/ps5/title` / `make -C platform/ps5/bench bench-title` + `title.sh install` are only needed after a rebuild (the install is skipped otherwise by the size check).
- Then reply **"done"**: read `$SPLAT_DATA_DIR/runs/console/` (newest first) plus the push `tee` file.

## Where this differs from the source docs

- `--verify` compares sizes only, so it runs **after** the real push; run before it, all three files report `DIFF` and the script exits 1.
- `run-on-console.sh` defaults to 120 s (`spike-b.md` §7); 30 s is used because the payload exits in ~2 s and the window is always spent in full.
- `run-bench.sh --payload` is `spike-a.md` §7's raw `run.sh … .stripped.elf` with `PS5_RUN_READY=<name>:` and `PS5_HTTP_TIMEOUT=300` pre-set (the stem `op_audit.stripped` never matches `op_audit: start`). The title route replaces that for the run.
- Nit in `push-data.sh` itself: its closing hint offers `run.sh platform/ps5/bench/op_audit.elf 1200` — the unstripped 150 MiB ELF and no readiness pattern. `run-bench.sh --payload` (71 MiB) is the correct call, and `--title` is the one that answers the GPU questions.
- The title gate reorders `spike-b-memory.md` §7's dev loop: that section's `#0 quit file`, `make`-then-upload, ShadowMountPlus scan, `klog` and launch are exactly what `title.sh` and `make -C platform/ps5/title` do, with the quit file written by `title.sh stop`. The bench title adds `title.sh put` for its selector file, inside the same `/data/ps5-splat-vr` policy.
- Every other command and flag in the four source docs exists and works as written: `run.sh <elf> [seconds]`, `PS5_RUN_READY`, `PS5_HTTP_TIMEOUT`, `klog.sh [--ensure] [FILE]`, `deploy.sh <elf>`, `push-data.sh --check|--dry-run|--verify [--scene DIR | --scenes DIR]`, `run-on-console.sh [seconds]`, `run-bench.sh [--title|--payload] <name> [seconds]`, `run-viewer.sh [--dry-run] [--scene NAME] [--seconds N] [--window N] [--env FILE]`, `run-vr-hello.sh [--dry-run] [--window N] [--package DIR]`, `make -C platform/ps5/{bench,hmd2,vr_hello}` + `make -C platform/ps5/bench bench-title` + `make -C platform/ps5/vr_hello vr-hello-title`, `make -C platform/ps5/viewer` + `platform/ps5/viewer/verify-package.sh <package>`, `platform/ps5/vr_hello/verify-package.sh <package>`, and now `make -C platform/ps5/title` + `title.sh {install,launch,stop,status,put} [--dry-run] <package|FILE REMOTE>`. `vr_hello` needs no `PS5_RUN_READY`: its start line embeds `vr_hello.elf` (payload) or `vrhello:` (title).

## Cleanup at the end of a session

The one test title on the home screen (`PPSA99202`: vk_hello, the bench, the
viewer and vr_hello, whichever was installed last) can be uninstalled
completely with one command. Read the plan first; it sends nothing:

```sh
platform/ps5/scripts/title-uninstall.sh PPSA99202
platform/ps5/scripts/title-uninstall.sh --yes PPSA99202
platform/ps5/scripts/title-uninstall.sh --yes --data PPSA99202   # also /data/ps5-splat-vr
platform/ps5/scripts/title-uninstall.sh --yes PPSA99770           # release files + tile only
# --data PPSA99770 is refused: /data/photo-splat-vr is the user's private data.
```

- `PPSA99202` is the dev identity (the id the dev build used before the Twiso
  collision). Its ownership proof accepts exactly the pairs `PPSA99202` +
  "Photo Splat VR Dev" (this build) and `PPSA99202` + "PS5 Splat VR Viewer"
  (the build installed before the dev name), so a console that still runs the
  older dev build is removable with the same command, while a Twiso install on
  that id is refused. `PPSA99770` is proved by `PPSA99770` + "Photo Splat VR".
  The script deletes exactly one folder plus the tile; `/data/ps5-splat-vr`
  stays unless `--data` is added, and `--data PPSA99770` is **refused**: that
  folder is the user's private data and nothing here deletes it.
- What a title leaves on the console, and what each piece belongs to:

  | Path / record | Created by | Removed by |
  | --- | --- | --- |
  | `/data/homebrew/<ID>/` (`eboot.bin`, `sce_module/`, `sce_sys/`, `radv-shader-cache/`) | `title.sh install` | this script (one delete job on that exact path) |
  | app.db rows (`tbl_contentinfo`, `tbl_appbrowse`) — the home-screen tile | ShadowMountPlus's register call | `sceAppInstUtilAppUnInstall`, asked for by this script |
  | `/user/appmeta/<ID>/` (`param.json`, `snd0.at9`, icons) | ShadowMountPlus (`sm_install.c:264-269`) | the same uninstall call; **not** deleted by this script |
  | `/user/app/<ID>/` (`sce_sys/`, `icon0.png`, `mount.lnk`) | ShadowMountPlus (`sm_install.c:242-306`) | the same uninstall call; **not** deleted by this script |
  | `/system_ex/app/<ID>` | nullfs mount of the source (`sm_filesystem.c:1327-1341`) | released before the uninstall (`sm_api_service.c:2641-2657`); the empty mount point may stay |
  | `/data/shadowmount/debug.log` lines, icon-cache variants | ShadowMountPlus | nothing — log/cache only, no state |

- The order is stop → unregister → delete → verify. "Stop" only writes
  `/data/ps5-splat-vr/title-quit` when *our* title is the running one (never a
  kill). "Unregister" is `POST /api/v1/games/uninstall {"title_id":…}` on
  ShadowMountPlus (10101), which unmounts the runtime layers and calls
  `sceAppInstUtilAppUnInstall` (`ShadowMountPlus/src/sm_api_service.c:2617-2680`)
  — the same call the console's own tile → Options → Delete makes. It is never a
  database edit by hand.
- Safety: only ids that are a `titleId` in
  `platform/ps5/title/identities.json` **and** whose installed `param.json`
  (homebrew folder, `/user/appmeta/<ID>` or `/user/app/<ID>/sce_sys`) carries
  that `titleId` and one of that identity's own names are touched, so Twiso on
  `PPSA99202` and another identity's name are both refused, and an id outside
  the table exits 2 before a request is built. The only tree deleted is exactly
  `/data/homebrew/<ID>` (no wildcard, no recursion of our own), plus exactly the
  dev identity's `/data/ps5-splat-vr` only with `--data` (scenes must then be
  pushed again — 1.36 GiB on this console). `--data` on the release id
  `PPSA99770` exits 2 before any request: `/data/photo-splat-vr` is never
  deleted. A release title that is running is never asked to quit through a
  file in its private folder — close it on the console instead.
- Verification: the full title list (`GET /api/titles`) and the
  `/data/homebrew` listing are snapshotted before and after into
  `$SPLAT_DATA_DIR/runs/console_cleanup/<UTC>-<mode>-{before,after}.json`, and
  the run fails unless the only differences are the requested ids — so run it
  when nothing else is installing a title, or a concurrent `title.sh install`
  shows up as an unexpected difference and the run fails (by design). A dry run
  takes both snapshots too and requires them to be identical. Then it reports
  `/user/app/<ID>` and `/user/appmeta/<ID>`: if the uninstall call leaves them
  behind the command exits 6 and names them; delete those on the console (tile →
  Options) if you want the directories gone too. The tile is already gone at
  that point, because the app.db row is.
- Exit: 0 unregistered and removed, nothing left behind; 1 a request failed or
  the before/after difference was not exactly the requested ids; 2 usage, an id
  that is not ours, or a console copy that is not ours; 3 no ProsperoMgr or no
  ShadowMountPlus answer (nothing was changed); 4 the unregister was refused or
  the title stayed in the list; 5 the title was running and would not stop; 6
  unregistered and removed, per-title leftovers remain (listed).
- **If it exits 3 at 10101**: ShadowMountPlus binds its API to `127.0.0.1`
  (`[CFG] api=127.0.0.1:10101` in `/data/shadowmount/debug.log`), so the
  uninstall request cannot be sent from the Mac — the same reason
  `title.sh install` always stops at its scan (`docs/BACKLOG.md`). Add
  `api_bind_address=0.0.0.0` (or the console's LAN address) to
  `/data/shadowmount/config.ini`, keeping every other line: the file is a
  template and the payload reloads it on its stamp, so the next scan picks the
  value up and the rerun works. Until then `--yes` refuses before the first
  write, and the folder is *not* removed: a tile whose source is gone is worse
  than a tile that is still installed.
- A later auto-scan cannot bring the title back: the source folder is gone.
  Nothing else on the console is touched — the three real titles
  (`PPSA01650` YouTube, `PPSA99198` Half-Life: Alyx, `PPSA99000/99008`) and
  every PS4 title stay exactly as they were, and the before/after check proves
  it.
- The run scripts have no `--uninstall-after` flag on purpose: it would have to
  be added to `run-bench.sh` (`platform/ps5/bench/`, outside the area this was
  built in) as well, and each run ends with its capture, so the cleanup is one
  command at the end of the session as above. Order it after the last run of the
  session — an uninstall in the middle costs a full `title.sh install`
  (25-73 MiB) before the next run.
