# platform/ps5 — payloads and console scripts

PS5-side payloads and the scripts that deploy them. The console addresses and
ports come from the environment or the repo's gitignored `.env.local`; nothing
here contains a host address.

| Setting | Default | Meaning |
| --- | --- | --- |
| `PS5_HOST` | — (required) | console LAN address |
| `PS5_DEPLOY_METHOD` | `prospero` | `prospero` = upload+launch through ProsperoMgr; `raw` = stream to elfldr |
| `PS5_PROSPERO_PORT` | `7070` | ProsperoMgr dashboard/API |
| `PS5_PORT` | `9021` | elfldr, for `PS5_DEPLOY_METHOD=raw` |
| `PS5_KLOG_PORT` | `3232` | console log stream (`klogsrv`; etaHEN's own forwarder is `9081`) |
| `PS5_KLOGSRV_PORT` | `3232` | port `klogsrv` binds; `klog.sh` starts it when closed |
| `PS5_KLOGSRV_PAYLOAD` | `klogsrv.elf` | payload name `klog.sh` launches for the log |
| `PS5_RUN_SECONDS` | `12` | `run.sh` capture window |
| `PS5_SHADOWMOUNT_PORT` | `10101` | ShadowMountPlus API; `title.sh install` asks it to rescan |
| `PS5_TITLE_WAIT` | `90` | `title.sh install` wait for the title to register |
| `PS5_TITLE_QUIT_WAIT` | `20` | `title.sh stop` wait for the title to end |
| `run-viewer.sh --seconds` | `60` | viewer title run length (`SECONDS=` in `viewer.env`) |
| `run-viewer.sh --scene` | first by name | scene folder under `/data/ps5-splat-vr/scenes/` (`SCENE=`) |

Payloads and the log server both go through ProsperoMgr's API on 7070, which
streams an ELF to elfldr on the console's own loopback (127.0.0.1:9021), so
**LAN port 9021 never has to be open**. ProsperoMgr's API is unauthenticated;
these scripts only ever use `GET /api/status`, `GET /api/elfldr/status`,
`GET /api/payloads`, `POST /api/payloads/upload?name=` and
`POST /api/payloads/launch`. No delete, rename, enable/disable, import-path,
PKG, FTP, process or shutdown route is called.

The title side (`title.sh`, below) uses a different, equally narrow set:
`GET /api/status`, `GET /api/titles`, `GET /api/titles/running`,
`POST /api/titles/launch`, and for a file copy `GET /api/ftp/list`,
`POST /api/ftp/mkdir`, `POST /api/ftp/upload` — all under
`/data/homebrew/<TITLE_ID>` and `/data/ps5-splat-vr`. It never deletes,
renames or changes permissions, and it never kills a process.

Payloads and the libraries they link:

| Path | Output | What it is |
| --- | --- | --- |
| `platform/ps5/hello/` | `hello.elf` | smoke test: start line, firmware, five ticks, done |
| `platform/ps5/hmd2/` | `libhmd2.a` | C7 PS VR2 bindings: runtime-loaded Hmd2/VrTracker2, VideoOut bus 32, reprojection frame path, CPU-filled eye buffers. A library, not a payload; built first by `vr_hello`'s Makefile |
| `platform/ps5/vr_hello/` | `vr_hello.elf` | PS VR2 test pattern (per-eye colour, grid, yaw marker), 1 Hz status, 30 s or headset removal. Built and verified statically; **not yet run on the console** |
| `platform/ps5/vk_hello/` | `vk_hello.elf`, `probe_*.elf` | RADV device/heap probe with one compute dispatch; the title variant is built from the same `main.cpp` |
| `platform/ps5/title/` | `build/ps5/title/<TITLE_ID>/` | the homebrew **title** package: `eboot.bin`, `sce_module/libc.prx`, `sce_sys/{param.json,icon0.png}` — see "Title build" below |
| `platform/ps5/viewer/` | `build/ps5/title/PPSA99202/` | the **viewer title**: a `.psplat` scene on the TV — C6 renderer + VideoOut, mono 1920x1080, sway inside the comfort radius. Package adds `shaders/*.spv` |

All of them build with the payload SDK outside the repo:

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/hmd2 && make -C platform/ps5/vr_hello
```

The runbook (expected log lines, what each failure means) is
`docs/research/spike-b-hmd2.md` section 8.

## run.sh — one-command dev loop

```sh
platform/ps5/scripts/run.sh platform/ps5/hello/hello.elf        # 12 s capture
platform/ps5/scripts/run.sh platform/ps5/hello/hello.elf 30     # 30 s capture
```

In order: `klog.sh --ensure` (starts `klogsrv` through ProsperoMgr when 3232 is
closed), connect to the log stream, `deploy.sh <elf>`, then capture the console
log for N seconds to stdout and to
`${SPLAT_DATA_DIR:-<repo parent>}/runs/console/<UTC timestamp>-<name>.log` (one
new file per run). Only the log goes to stdout; `deploy.sh`'s own output is
prefixed `deploy|` on stderr.

The **payload start line** is the first log line that names the payload, matched
case-insensitively against the ELF's stem (`hello.elf` → `hello`). Kernel log
lines carry the payload's process name, and a payload normally prints its own
name in its first line. ProsperoMgr's `POST /api/payloads/...` bookkeeping lines
are ignored, so `name=hello.elf` in them cannot count. Set
`PS5_RUN_READY=<extended regex>` to use another pattern.

| Exit | Meaning |
| --- | --- |
| 0 | start line seen |
| 1 | no start line within N seconds |
| 2 | usage error |
| 3 | no log stream (ProsperoMgr/klogsrv unavailable) |
| 4 | deploy failed |
| 5 | the log stayed empty for the whole window |

## deploy.sh

```sh
# default: upload to ProsperoMgr (7070) and launch it there. ProsperoMgr streams
# the ELF to elfldr on the console's own loopback, so 9021 can stay LAN-closed.
platform/ps5/scripts/deploy.sh platform/ps5/hello/hello.elf

# raw: send the ELF to elfldr on $PS5_HOST:9021 and print the payload's stdout.
PS5_DEPLOY_METHOD=raw platform/ps5/scripts/deploy.sh platform/ps5/hello/hello.elf
```

`prospero` performs, in order: `GET /api/status` (must be ProsperoMgr),
`GET /api/elfldr/status` (must report `reachable:true`, otherwise it stops
before uploading anything), `POST /api/payloads/upload?name=<file>`, then
`POST /api/payloads/launch` with `{"name":"<file>"}`. A payload launched this
way does **not** return its stdout — its `klog_puts()` lines show up on the klog
stream. Endpoint details and formats are in
`docs/research/ps5-console.md` section 7.

The file is stored on the console under `/data/ProsperoMgr/payloads/` with its
basename, so `hello.elf` overwrites an earlier `hello.elf`.

## klog.sh

```sh
platform/ps5/scripts/klog.sh                        # live, to the terminal
platform/ps5/scripts/klog.sh console.log            # live + append to a file
platform/ps5/scripts/klog.sh --ensure               # start the log server only
PS5_KLOG_SECONDS=20 platform/ps5/scripts/klog.sh console.log   # stop after 20 s
```

Default source is the **klogsrv** payload on 3232. If nothing listens there, the
script launches `klogsrv.elf` through ProsperoMgr (`POST /api/payloads/launch`,
after `GET /api/status` and `GET /api/payloads` to check the service and that
the payload exists and is enabled), then waits up to 10 s for 3232 to accept.
ProsperoMgr already bundles `klogsrv.elf`, so nothing needs uploading; to build
your own copy, use `$SPLAT_DATA_DIR/ref/klogsrv` (`make -f Makefile.ps5`) and
upload it through ProsperoMgr's Payloads page.

`PS5_KLOG_PORT=9081` reads etaHEN's klog forwarder instead; `klog.sh` only ever
starts klogsrv, never etaHEN. The script probes the port first with a plain TCP
connect (python3, no `nc -G`), so a closed port is reported immediately instead
of hanging, and it prints `klog: streaming <host>:<port>` once the stream is
connected (`run.sh` waits for that marker).

klogsrv forwards `/dev/klog` only while a client is connected and starts at the
current end of the log: lines written before you connect are not replayed.

## Title build (`platform/ps5/title/`)

The app ships as a **homebrew title**, not a payload: a folder that
ShadowMountPlus registers and the home screen starts. A title gets the
application pool (11.4-12 GiB of direct memory), which is what RADV turns into
its device heap; a payload has budget 0 and therefore no heap at all
(`docs/research/spike-b-memory.md` section 1).

```sh
make -C platform/ps5/title                  # dev build:  build/ps5/title/PPSA99202/
make -C platform/ps5/title RELEASE=1        # release:    build/ps5/title/PPSA99770/
make -C platform/ps5/vk_hello vk_hello-title   # same build, from the program's dir
```

The package holds `eboot.bin` (signed fSELF), `sce_module/libc.prx`,
`sce_sys/param.json` and `sce_sys/icon0.png`. The recipe, every input and the
title ID decision are in `platform/ps5/title/README.md`.

**Two installs: dev `PPSA99202`, release `PPSA99770`** (user decision,
`docs/PLAN.md` "One app per install on the console"). Every program this repo
packages — vk_hello, the bench title, vr_hello and the viewer — is the same title
per identity: one `param.json` template, one tile per identity, and an install
updates that tile in place instead of adding an app. One build variable picks
the identity (`platform/ps5/title/identities.json`, the only place an id, a
title name or a data folder is written):

```sh
make -C platform/ps5/viewer               # dev build   -> build/ps5/title/PPSA99202
make -C platform/ps5/viewer RELEASE=1     # release     -> build/ps5/title/PPSA99770
make -C platform/ps5/viewer TITLE_ID=PPSA99770   # the same, selected by id
```

The package's `sce_sys/param.json` is generated at package time from the
identity-free `platform/ps5/title/param.json`, and every script resolves the
same table (`ps5_title_id`, `ps5_title_data_dir` in `ps5-env.sh`), so
`/data/homebrew/<ID>`, the build directory and the installed `param.json`
cannot disagree. The identity carries the title name ("Photo Splat VR Dev" /
"Photo Splat VR"), the generated icon and the PS VR2 attributes
`0x62000000`/`0x4080c40`, so the system sees a VR title and a non-VR program
simply never opens Hmd2. Both ids are `PPSA99xxx` homebrew ids: not PS5LM
`PPSA99575`/`PPSA99581`, PS5VR `PPSA99177`/`PPSA99176`, PS5_Vulkan
`PPSA99014`/`PPSA99015`/`PPSA99170` or its runner `PPSA99988`, and `title.sh`
refuses those. `contentId` is exactly 36 characters
(`IV0000-PPSA99202_00-PHOTOSPLATVRDEV0` for the dev build,
`IV0000-PPSA99770_00-PHOTOSPLATVR0001` for the release); no Apple name or mark
appears in the name or the icon (spec, Licensing rule 5).

**The two installs share no data.** The dev build keeps `/data/ps5-splat-vr`
(all test scenes, models and logs); the release build uses the user's private
`/data/photo-splat-vr` and downloads its own models. The viewer and its helper
compile the folder in from the identity (`SPLAT_DATA_ROOT`), so the helper's
mailbox, pid lock and inbox are per identity too. No script in this repo may
read, list, write or delete under `/data/photo-splat-vr`, and none may launch
`PPSA99770`: `ps5-env.sh` refuses both before a request is built, `title.sh
install/status` (which only touch `/data/homebrew/PPSA99770`) still work, and
`title-uninstall.sh --data PPSA99770` is refused. See AGENTS.md.

`platform/ps5/scripts/title.sh install|launch|stop|status <package dir>`:

```sh
platform/ps5/scripts/title.sh install --dry-run build/ps5/title/PPSA99202   # every request, sends nothing
platform/ps5/scripts/title.sh install build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh launch  build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh status  build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh stop    build/ps5/title/PPSA99202
```

`install` uploads the folder to `/data/homebrew/<TITLE_ID>/` over ProsperoMgr's
file API, verifies each size, then asks ShadowMountPlus (`POST /api/v1/scan`,
10101) to register and mount it; `launch` uses `POST /api/titles/launch` on 7070
and needs a foreground user signed in; `stop` writes the quit file
`<dataDir>/title-quit` and never kills; `status` is read-only. `launch`, `stop`
and `put` are refused for `PPSA99770` (agent rule); `install` and `status` work
for it.
`--dry-run` works with no console and no `PS5_HOST`. Exit codes: 0 done,
1 a request failed, 2 bad/unsafe package, 3 no ProsperoMgr or ShadowMountPlus,
4 not registered / launch refused, 5 the title is (or stays) running.

Build-only so far: nothing of this has run on the console yet — the gate is
`docs/console-runbook.md` section 1.

## The viewer title (`platform/ps5/viewer/`) — splats on the TV

The first visible result: a homebrew title that loads a `.psplat` scene and
draws it on the TV with the project's own renderer (`core/render`: C6
`SplatRenderer`, `ViewerState`, comfort fade), mono at 1920x1080, with a slow
side-to-side sway inside 60 % of the comfort radius so parallax shows. It is a
title, not a payload, for the direct-memory reason above (RADV has no heap in a
payload).

```sh
platform/ps5/ggml/build.sh                    # once: ggml + core (core/render included)
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/viewer                   # build/ps5/title/PPSA99202/ (26.6 MiB)
platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99202

platform/ps5/scripts/push-data.sh --scenes "$SPLAT_DATA_DIR/runs/render/comfort/scenes"
platform/ps5/scripts/run-viewer.sh            # install if needed, launch, capture, stop
platform/ps5/scripts/run-viewer.sh --dry-run  # every request, sends nothing
```

- **The viewer is the one app per identity, dev `PPSA99202`** (release
  `PPSA99770`) — the same package the benches and vr_hello install, so
  `run-viewer.sh` puts the viewer's eboot back after another test replaced it;
  `platform/ps5/title/identities.json` names the identities, and `title.sh`
  refuses the other projects' ids.
- **No Vulkan loader.** RADV's archive exports one symbol, so
  `vk_entrypoints.cpp` forwards the ~80 `vk*` names the renderer calls (one
  forwarding definition each, resolved through RADV's ICD entry as a loader
  would). They stay local to the title.
- **Presentation** is `videoout.cpp`: two direct-memory framebuffers registered
  with `libSceVideoOut` (1920x1080, `B8G8R8A8_SDR`, tiling 0 = 64 KiB R_X
  tiles), the same layout PS5_Mesa's VideoOut WSI and PS5_Vulkan's demo renderer
  write. RADV has no WSI here, so each frame is copied out of the renderer's
  image into a host-visible buffer and written into the tiles by the CPU (that
  cost is logged as `copy=`/`tiles=`); a GPU tile pass is in `docs/BACKLOG.md`.
- **Input**: none. A title launched through ProsperoMgr gets no arguments, so
  the run length and the scene come from `/data/ps5-splat-vr/bench/viewer.env`
  (`SECONDS`, `SCENE`, `WIDTH`/`HEIGHT`, `FLIP_RATE`, `COMFORT_RADIUS`,
  `SWAY_PERIOD`, plus any `RADV_DEBUG`/`MESA_*` setting). `run-viewer.sh`
  writes it with `title.sh put`; DualSense input is a backlog item.
- **Exit**: after `SECONDS`, or on the quit file `title.sh stop` writes, through
  `sceSystemServiceLoadExec("exit", nullptr)`. Every GPU wait is bounded; a
  timeout logs `leaking the renderer, its buffers and the device` and frees
  nothing (vk_hello's teardown policy).

The runbook step, with the expected klog lines and what each failure means, is
`docs/console-runbook.md` section 1b. Build-only so far: the console was off,
so the package is built and statically checked (`verify-package.sh` PASS: signed
fSELF, integrity valid, 6 SPIR-V modules, no exported symbol, VideoOut needed).

## hello payload

`platform/ps5/hello/` is the smoke test: it prints a start line, the firmware,
five one-second ticks and a done line, each to stdout and to `klog_puts()`
(`<ps5/klog.h>`). Build with the payload SDK outside the repo:

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/toolchains/ps5-payload-sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/hello
```

Launched through ProsperoMgr its kernel-log lines carry the loader process name,
not the file name:

```
<118>[payload.elf] hello: start (ps5-splat-vr)
<118>[payload.elf] hello: firmware 13.60 (kern.sdk_version 0x13600007)
<118>[payload.elf] hello: done, exit 0
```

`hello.elf` is a build artifact and is gitignored (`hello/.gitignore`); the
Makefile produces it from `main.cpp`.

## What this project has put on the console

Recorded 2026-10-26 (Task A4). Nothing was deleted, renamed, disabled,
installed as a PKG, or changed in settings or firmware.

| File | Location on the console | Origin | Purpose |
| --- | --- | --- | --- |
| `klogsrv.elf` | `/data/ProsperoMgr/bundled/klogsrv.elf` | ProsperoMgr's own bundled copy (unmodified); launched by `klog.sh` | `/dev/klog` → TCP 3232 |
| `hello.elf` | `/data/ProsperoMgr/payloads/hello.elf` | uploaded by `deploy.sh`, 119184 bytes | smoke test |
| `PPSA99202/` | `/data/homebrew/PPSA99202/` | `title.sh install` | the dev homebrew title — vk_hello, benches, vr_hello or the viewer, whichever was installed last (25-73 MiB); its data stays in `/data/ps5-splat-vr` |
| `PPSA99770/` | `/data/homebrew/PPSA99770/` | `title.sh install` (RELEASE=1) | the release homebrew title; the user's own install, data in `/data/photo-splat-vr` (agents never launch it) |

Both payloads are in ProsperoMgr's registry; `klogsrv.elf` stays running as
`klogsrv.elf` (a normal process, gone after a console reboot) and is relaunched
by `klog.sh`/`run.sh` whenever 3232 is closed. `hello.elf` is overwritten by each
deploy. The title folder is a plain folder copy — remove it from the console
only with ProsperoMgr's file page, never from these scripts. Anything else a
later payload run adds (`vk_hello.elf` and the bench ELFs) lands in
`/data/ProsperoMgr/payloads/` the same way.
