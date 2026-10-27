# PS5 console: ports, payload SDK, and how the reference projects build

Research for Task A, 2026-10-25. Host: macOS 26.5, arm64 (Apple silicon).
Console: jailbroken PS5 on the LAN, `PS5_HOST` from `.env.local` (not recorded
here), firmware 13.60 assumed.

## 1. Console probe (read-only)

`ping` answers in 0.5 ms, so the console is on the network. Every probed TCP
port refused the connection (macOS `nc -z -G 2 -w 2`, and a Python
`socket.connect` with a 2 s timeout, same result):

| Port | Refused? | What it is |
| --- | --- | --- |
| 21 | yes | standard FTP port; nothing on a stock console |
| 1337 | yes | etaHEN's optional FTP server |
| 2121 | yes | [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) payload FTP |
| 9020 | yes | exploit-stage ELF loader (BD-J / WebKit payload loader) |
| 9021 | yes | [elfldr](https://github.com/ps5-payload-dev/elfldr) payload loader |
| 9081 | yes | etaHEN kernel-log forwarder (`klog`) |
| 9090 | yes | etaHEN Direct PKG Installer service (DPI v1) |
| 8080 | yes | [websrv](https://github.com/ps5-payload-dev/websrv) |
| 12800 | yes | etaHEN DPI v2 HTTP endpoint |
| 3232 | yes | [klogsrv](https://github.com/ps5-payload-dev/klogsrv) (`/dev/klog` → socket) |
| 8082 | **open** | a homebrew web UI (`<title>GarlicMgr</title>`, HTTP 200) |

So at probe time the console was up and running one homebrew payload, but
**neither an ELF loader nor a log stream was listening**. Nothing can be
deployed or captured until one is started on the console (step 5 below).

**Superseded 2026-10-26:** the console now exposes a different port set
(7070, 8082, 9046, 9295, 41800) and no loader at all. See section 6 for the
current state; the conclusions there replace this table.

Port identification sources: elfldr README (9020 for the exploit loader, 9021
for elfldr), ftpsrv README (2121), klogsrv README (3232), websrv README (8080),
[etaHEN README](https://github.com/etaHEN/etaHEN) (klog 9081, optional FTP
1337), and for 9090/12800 etaHEN's DPI v1/v2 package-installer services
([PSX-Place etaHEN thread](https://www.psx-place.com/threads/update-v1-8b-etahen-aio-homebrew-enabler-for-the-ps5.42050/),
[PS5_Jailbreak DPI v2 thread](https://www.reddit.com/r/PS5_Jailbreak/comments/1mpa1y6/install_package_via_ethernet_very_slow_with/)).

What the user must start on the console:

1. **elfldr** (port 9021). Load it from Payload Manager / WebKit Autoloader, or
   send `elfldr-ps5.elf` to the exploit loader on 9020.
2. **A log stream**: either **etaHEN** with Klog enabled (9081) or the
   **klogsrv** payload (3232, sent to 9021 like any payload). etaHEN's klog is
   off by default (`Klog = 0` in its settings).
3. Optional: **ftpsrv** (2121) for file uploads.

`platform/ps5/scripts/klog.sh` defaults to etaHEN's 9081 and streams raw text;
`deploy.sh` sends to 9021 in raw mode and to ProsperoMgr's 7070 in prospero mode
(section 7).

## 2. Building PS5 payloads on macOS arm64

The ps5-payload SDK's release zip is host-architecture independent: its README
says the binary distribution "should work regardless of CPU architecture, e.g.,
x86_64, aarch64". Verified here on arm64.

Prerequisites (from the SDK README, macOS section, and PS5LM's
`scripts/setup-sdk.sh`): `brew install llvm lld socat` (PS5LM pins
`llvm@21`), then `export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config`.
The SDK's `prospero-clang` wrapper calls the host `clang` with
`-target x86_64-sie-ps5`, the SDK sysroot, `crt1.o`, `-lc`, `-lkernel_web`,
`-lSceLibcInternal` and `-lSceNet`; C++17 works because `target/user/homebrew`
carries libc++.

Installed here:

- SDK `v0.45` (released 2026-10-21), release zip
  `ps5-payload-sdk.zip`, sha256
  `5fac64093d0e3e69f2584ce7935f24f703f622d9681f1495c85b9f00d1291a2c`,
  unzipped to `ref/toolchains/ps5-payload-sdk`.
- `llvm@21` 21.1.8, `lld` 23.1.4, `socat` 1.8.1.3 from Homebrew.

**Gotcha:** the v0.45 zip ships `bin/clang`, `bin/clang++`, `bin/ld.lld` and
`bin/llvm-*` as plain copies of the `prospero-llvm` script instead of symlinks
(the upstream `host/Makefile` target `install-symlinks` creates them as
symlinks). Run as copies, `prospero-clang` falls through to
`$LLVM_BINDIR/llvm-clang` and dies with
`clang: line 45: /llvm-clang: No such file or directory`. Fix (done here):

```sh
cd $PS5_PAYLOAD_SDK/bin
for n in clang clang++ clang-cpp ld.lld llvm-ar llvm-nm llvm-objcopy llvm-ranlib llvm-strip; do
    ln -sf prospero-llvm $n
done
for n in ar nm objcopy ranlib strip; do ln -sf llvm-$n prospero-$n; done
ln -sf prospero-clang prospero-as
```

Build of the hello payload: `platform/ps5/hello/Makefile` includes
`$(PS5_PAYLOAD_SDK)/toolchain/prospero.mk`; output is an x86-64 FreeBSD PIE
`hello.elf`. `prospero-lld` looks for `ld.lld` under
`/opt/homebrew/opt/lld/bin`, so `brew install lld` is required in practice, not
just `llvm`.

Firmware in a payload: `sysctlbyname("kern.sdk_version", ...)` returns
`0x13600000` for 13.60 (PS5LM-Plus `ps5/app/main.cpp` formats it as
`%x.%02x`). `sceKernelGetSystemSwVersion` exists in `libkernel` but returns a
text build number (`13.590.001` on 13.00), not the marketing version, so the
hello payload prints both.

**stdout vs klog:** elfldr redirects the payload's stdin/stdout/stderr to the
loader socket (`socksrv.c` defaults to `pipe=1`, `elfldr.c` `dup2`s it), so
`printf` comes back over port 9021 and does *not* appear in `/dev/klog`. A
payload that should show up in the klog stream calls the SDK's
`klog_puts`/`klog_printf` (`<ps5/klog.h>`, kernel-log syscall 0x259, prefix
`<118>[<process name>]`, `crt/klog.c`). The hello payload writes every line to
both.

## 3. How PS5LM / PS5LM-Plus deploy and log

Both are GPL-3.0 and use ps5-payload-dev tools.

- **Payload** (`ps5lm.elf`): `scripts/send.sh` does
  `socat -t 9999999 - TCP:$PS5_HOST:9021 < payload.elf`; elfldr keeps the
  connection open, so the payload's stdout streams back to the PC. This is the
  simplest live-log path and needs no klog at all.
- **Files**: ftpsrv on 2121, upload with `curl -T file ftp://$PS5_HOST:2121/...`.
- **Native title** (PS5LM and Plus): `scripts/ps5lm-app.sh close|deploy|launch`:
  quit request via a file over FTP, upload the title folder, then send
  ps5-homebrew-dev-protocol's launch payload to elfldr 9021 with socat. The app
  logs to `/data/PS5LM/app.log` and `llama.log` on the console.
- **Kernel log**: `scripts/console-setup.sh` sends ftpsrv 0.21.1, shsrv 0.20
  and klogsrv 0.9 to 9021; klogsrv then serves `/dev/klog` on **3232**
  (`nc $PS5_HOST 3232`). PS5LM's console notes list the autoload set
  (kstuff-lite, ShadowMountPlus, etaHEN, ftpsrv, shsrv, klogsrv).
- **PS5LM Plus** also reads klogsrv 3232 into its debug report (last 1 MiB) and
  notes that klogsrv sends every line to every client, so a PC capture can stay
  connected. etaHEN's own forwarder on 9081 is the alternative when klogsrv is
  not loaded.

Sources: `PS5LM/docs/BUILDING.md`, `PS5LM/docs/CONSOLE.md`,
`PS5LM/docs/PAYLOAD.md`, `PS5LM/scripts/{send,run,console-setup,ps5lm-app}.sh`,
`PS5LM-Plus/README.md`, `PS5LM-Plus/docs/CONSOLE.md`.

## 4. PS5VR: license and GPU API

- **License: GPL-3.0** (`PS5VR/LICENSE` is the GPLv3 text;
  `THIRD_PARTY_NOTICES.md` says "GPLv3.0 or later"; the vendored engine is
  GPL-3.0-or-later in `app/engine/LICENSE.ENGINE`). Compatible with this
  project's GPL-3.0-or-later, but copying code still needs the usual source/notice
  handling.
- **GPU API: AGC, not Vulkan.** PS5VR renders with Sony's low-level AGC API
  (`libSceAgc` / `libSceAgcDriver`, declared clean-room in
  `app/engine/media/include/sce/sce_agc.h`; the renderer is
  `app/engine/media/src/eng_agc_runtime.c`). There is no Vulkan use anywhere in
  `app/src` or `app/engine`.
- **How buffers reach Hmd2:** through `libSceVideoOut`, opened with **bus type
  32**, which `app/src/app_vr.c` calls Hmd2's "VR driver"
  (`sceVideoOutOpen(0xff, 32, 0, NULL)`). It then:
  - `sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000, tiling=0,
    2*EYE_W, EYE_H, ...)` — one side-by-side eye texture, not two;
  - `sceVideoOutRegisterBuffers2(vh, 0, 0, bufs, 3, attr, category=0x10, NULL)`
    — three buffers, each with a layer list;
  - a layer of **type 0x10** that carries the same texture twice, once per eye,
    with per-eye viewports and FOV tangents;
  - `sceHmd2ReprojectionBeginFrame/GetPredictedDisplayTime/SetParam`,
    `sceVideoOutConfigureOutput(vh, 0x2000f)` (120 Hz), then
    `sceVideoOutSubmitFlip(vh, buf, 2, frame)`.
  Eye buffers default to 4096×4096 per eye (8/10/16-bit), 3 buffers, direct
  memory.
- Hmd2/VrTracker2 functions come from `sceSysmoduleLoadModule` +
  `sceKernelDlsym`, or fixed offsets per firmware with a rodata anchor check
  (`app/src/vr_exports*.h`, `docs/firmware-support.md`).

**Implication for C7 / Spike B:** Hmd2 consumes a `libSceVideoOut` buffer with
a VideoOut attribute (format `0x8000...`, linear tiling), not a `VkImage`. RADV
images would have to be linear and exported as that buffer, or copied into one.
This is the interop unknown the spec already flags.

## 5. PS5LM's RADV build and Docker on Apple silicon

PS5LM does **not** ship a Dockerfile and its build doc does not mention Docker.
The documented path is a native **x86-64 Linux** host (Arch/CachyOS supported,
Fedora 44 also works):

```sh
mkdir -p .deps/src && cd .deps/src
git clone https://github.com/mihawk-99/PS5_Vulkan
git clone https://github.com/mihawk-99/PS5_Mesa
git clone https://github.com/mihawk-99/PS5_PayloadSDK
(cd PS5_Mesa && git am ../../../patches/mesa/*.patch)
(cd PS5_Vulkan && bash tools/setup-native-dependencies.sh &&
  PS5_MESA_REVISION=$(git -C ../PS5_Mesa rev-parse HEAD) bash tools/build-radv.sh release && make app)
```

`tools/build-radv.sh` builds the Mesa fork (Mesa 26.2.0 + PS5 winsys) with
meson and a PS5 cross file into `libvulkan_radeon.ps5.a` (RADV + ACO + NIR +
Mesa's Vulkan runtime), release variant for titles; host packages include
clang/llvm/lld, meson, ninja, python-mako, glslang, libclc, SPIRV-LLVM-Translator.
Titles link the archive directly, no Vulkan loader. PS5LM's CI pins PS5_Vulkan
`f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed` and the PS5_PayloadSDK fork
`b83202be73e930050e00de0f1b4d9ec46c0391bf`; PS5_Vulkan's `build-radv.sh`
defaults to PS5_Mesa `1ba9874f478657ccf6da72ce55f0260164cc0bd0` (its `main`).

The only container recipe is CI: `.github/workflows/native-build.yml` runs the
same build in a digest-pinned Debian trixie image with `llvm-19`,
`BUILD_JOBS=2`, via `scripts/test-native-build.sh` (`PS5LM/docs/CI.md`). It is
an x86-64 Linux build environment, not an artefact that runs on the Mac.

Can that run on Apple silicon? Only as an **amd64 container under emulation**
(Docker Desktop's Rosetta 2 backend, or qemu). PS5VR proves the pattern: its
`app/scripts/build.sh` re-execs `docker run --rm --platform linux/amd64` with
an `engine/ps5-dev:llvm18-sdk-v0.42` image, and its
`app/scripts/build-native-mac.sh` says the native Mac build is "several times
faster than the emulated x86 container". So: technically possible, slow, and
not a supported path; there is no PS5LM Dockerfile to start from. The payload
SDK itself needs no Docker on arm64 (section 2). For RADV and title packaging,
use an x86-64 Linux machine or the CI image.

## 6. Loader ports on this console (2026-10-26, Task A2)

The console changed since section 1: a full TCP scan now shows **7070, 8082,
9046, 9295, 41800** open, and nothing else (9020, 9021, 9027, 9081, 3232,
1337, 2121, 2323, 8080, 8084, 9090, 12800, 10101 all refused). None of the
open ports sends a banner; a read-only probe (connect, send nothing, read)
got zero bytes in 12 s from every one of them, and the connection stayed
open. Raw output: `runs/A2/port_probe.txt`.

### 6.1 Per-port identification

| Port | Who | Protocol it expects | Source |
| --- | --- | --- | --- |
| 7070 | **ProsperoMgr** (web dashboard + JSON API) | HTTP/1.1; browser UI and `POST /api/...` calls | [notmaj0r/ProsperoMgr](https://github.com/notmaj0r/ProsperoMgr) `source/backend/include/config.hpp` (`kHttpPort = 7070`), `README.md` |
| 8082 | **GarlicMgr** / `garlic-savemgr` v1.14 | HTTP/1.1; PS5 save decrypt/encrypt/browse UI | [earthonion/garlic-savemgr](https://github.com/earthonion/garlic-savemgr) `src/main.c` (`#define PORT 8082`) |
| 9046 | **ProsperoMgr PKG direct upload** | RFC 6455 WebSocket on `/ws/upload`, then binary PKG frames (one segment per frame) | ProsperoMgr `source/backend/src/pkg_ws_upload.cpp` (`kPort = 9046`, `kPath = "/ws/upload"`), `frontend/src/lib/pkgDirectUpload.js` |
| 9295 | **Sony Remote Play** (`SceRemotePlay`) | proprietary binary; the client speaks first | Sony Remote Play port lists (TCP 9295 with 9296/9297, 9302-9304 UDP) — see section 6.4 |
| 41800 | **not identified** — not homebrew | silent; accepts TCP only | not in <https://psxtools.de/forum/index.php?thread/92971-ps5-payload-port-guide/>, nor in ProsperoMgr or garlic-savemgr sources |

Cross-check with the psxtools.de PS5 port table (community, updated
2026-10): 7070 = "Prospero Manager, Web-UI", 8082 = "Garlic Save Manager",
9046 is absent there (it is ProsperoMgr's second socket, not in that list),
and 41800 is absent everywhere.

### 6.2 GarlicMgr: a save manager, not a loader

Fetched with GET only into `runs/A2/` (`root.html`, 213 KB, single file with
inline CSS/JS — there are no separate bundles; `root.headers` shows
`Content-Type: text/html`, `Server` header absent, `Access-Control-Allow-Origin: *`).

- Single payload by **earthonion** ([garlic-savemgr](https://github.com/earthonion/garlic-savemgr),
  "PS5 save decrypt/encrypt/browse with embedded web UI"). Its own README:
  "1. Send `garlic-savemgr.elf` to elfldr. 2. Open `http://<ps5-ip>:8082`".
  So GarlicMgr is a *client* of elfldr, never a loader itself.
- State lives in `/data/garlic/` (`cloud.json`); `GET /api/users` returns the
  console profile (`{"users":[{"id":"<hash>","name":"<local profile name>"}]}`,
  redacted here), `GET
  /api/settings/cloud` returns `config_error: "Cannot read /data/garlic/cloud.json"`,
  i.e. cloud backup is unconfigured.
- **No mention** of 7070, 9046, 9295, 41800, klog, klogsrv or payload
  sending anywhere in the page. The endpoint list is saves/mount/files/import/
  resign/cloud/editor/shutdown only (`/api/saves`, `/api/mount`, `/api/files`,
  `/api/settings/cloud`, `/api/shutdown`, ...). No `/api/payload*`, no
  `/api/klog*`.
- Its "Terminal" panel is **client-side**: `log()` (`root.html` line 895)
  appends DOM nodes to `#log` in the browser. It shows this web UI's own
  actions, not the console kernel log.
- Consequences: nothing in GarlicMgr can load a payload or stream a log, and
  GarlicMgr has no port setting to change.

### 6.3 7070 and 9046 are one program: ProsperoMgr

[ProsperoMgr](https://github.com/notmaj0r/ProsperoMgr) (GPL-3.0, "maj0r",
v1.1) is a single ELF that serves a full dashboard from 7070: files, ZIP/RAR,
payload management, PKG install, saves, processes, temperatures, logs.

How it loads payloads — **this is the port the user is looking for, and it is
not 7070**:

```c
// source/backend/include/config.hpp
static constexpr uint16_t kHttpPort   = 7070;
static constexpr const char *kElfldrHost = "127.0.0.1";   // <-- loopback
static constexpr uint16_t   kElfldrPort  = 9021;
static constexpr uint16_t   kKlogsrvPort = 3232;
```

`ElfldrClient` opens a TCP connection to `127.0.0.1:9021` and streams the ELF
(`source/backend/src/elfldr_client.cpp`, `include/elfldr_client.hpp` — "the
elfldr wire protocol (127.0.0.1:9021), same as every loader in the ecosystem").
`POST /api/payloads/launch` is the UI action behind it.

Other facts that matter for us:

- It **bundles** `ftpsrv.elf`, `klogsrv.elf` and `prospero-pkgd.elf` inside the
  ELF (`source/payloads/`, embedded by `tools/gen_payloads.py`) and drops them
  in `/data/ProsperoMgr/bundled/` at startup, but **launches them through
  elfldr** like any other file. Bundled autostart is off unless
  `/data/ProsperoMgr/bundled_autostart.txt` exists (`autoloader.cpp`).
- Its **Logs** page has a klog tab that tails `127.0.0.1:3232`
  (`klog_client.cpp`, `GET /api/klogs` → `connected: false` until klogsrv
  runs). It never starts klogsrv by itself.
- 9046 is a **separate LAN-facing listener** for PKG streaming: WebSocket
  handshake (`Sec-WebSocket-Accept` with the RFC 6455 GUID), then binary
  frames. Comment in `pkg_ws_upload.cpp`: "Reachable from the LAN (a PC
  browser), unlike the loopback-only pkg_stream port" (the loopback one is
  9045, `pkg_stream.cpp`). An ELF sent to 9046 is not an HTTP request, let
  alone a WebSocket handshake, so it is dropped.
- **Security note (matters for the public repo and for our workflow):** the
  whole API is unauthenticated by design — "same trust model as the rest of
  ProsperoMgr's unauthenticated local API" (`pkg_ws_upload.cpp`). Any host on
  the LAN can `POST /api/payloads/launch`, `/api/shutdown`, `/api/ftp/delete`
  etc. We must never script those; the user drives the UI.

Identification confidence: 9046 = ProsperoMgr is very high (the number is not in
the community port table and appears in exactly one PS5 project we found, its
WebSocket). 7070 = ProsperoMgr is near-certain (its documented HTTP port, paired
with 9046, with GarlicMgr already accounting for 8082). Per Task A2's safety
rules we did not GET 7070; opening `http://$PS5_HOST:7070` in a browser
confirms it in one second.

### 6.4 9295 and 41800

- **9295** is Sony's Remote Play control port (TCP; streaming uses UDP
  9296/9297 and 9302-9304). It is not homebrew and does not answer before the
  client sends the Remote Play handshake, which is why it looks like a silent
  loader. Source: Sony's Remote Play port lists and the community port
  threads (<https://remoteplay.dl.playstation.net/remoteplay/lang/en/1100006.html>,
  <https://www.reddit.com/r/PS5/comments/kx8mhy/>). `shsrv`'s `ps` output lists
  `SceRemotePlay` as a stock system process, so the port is expected on any
  console with Remote Play enabled.
- **41800** stays unidentified. Evidence: silent on connect (12 s, no bytes,
  no close), absent from the psxtools PS5 table (39 rows), absent
  from ProsperoMgr and garlic-savemgr sources, absent from every
  ps5-payload-dev tool, and not a port any of the user's known payloads use.
  Its behaviour matches a Sony system service (same profile as 9295), not a
  loader. Two safe checks the user can run, neither of which sends anything:
  1. Settings -> System -> Remote Play -> disable, rescan; if 41800 closes it
     is part of the Remote Play stack.
  2. With a shell on the console (`shsrv` on 2323 gives `ps`; it has no
     netstat/sockstat), compare the non-Sony processes with the port set.

Rule for both: do not send anything to them.

### 6.5 Why klogsrv never started after the ELF went to 7070

Sending `klogsrv-ps5.elf` to 7070 only proves that ProsperoMgr's HTTP server
accepted a TCP connection. The first bytes of the payload are
`\x7f E L F \x02 \x01 \x01 ...`, which is not a request line. ProsperoMgr
serves HTTP through libmicrohttpd (`MHD_*` calls in `http_server.cpp`), which
answers such a stream with 400/414 and closes the connection. Nothing ever
looked for an ELF, so no process was created and nothing bound 3232.

Three independent confirmations:

1. There is no ELF loader on any of the five open ports: 7070/9046 are
   ProsperoMgr's HTTP and WebSocket sockets, 9295 is Sony's, 41800 is not a
   loader (6.4). The real loaders on this console family are 9020 (BD-J stage,
   not present), 9021 (elfldr, closed) and 9027 (etaHEN's own/ps5debug
   loader, closed).
2. `klogsrv` binds 3232 itself when it runs (`klogsrv/main.c:312`,
   `uint16_t port = 3232`, overridable with `-p`). Port 3232 is **refused**,
   so the process never existed.
3. The user's premise "7070 is the payload channel" is wrong for this
   console: ProsperoMgr's payload channel is **9021 on loopback**, and it can
   only work while elfldr is running.

Nothing was damaged: the ELF was read as a bad HTTP request and discarded.

### 6.6 The exact safe way to load an ELF here

Loading an ELF always needs two things: a running ELF loader, and raw ELF
bytes sent to *that* loader's port. On this console only the first is
missing.

**Preferred path (keeps 9021 off the LAN):** open `http://$PS5_HOST:7070` in
a browser, Payloads -> upload (or pick the bundled `klogsrv.elf`) -> Launch.
ProsperoMgr streams the file to `127.0.0.1:9021`, so a firewall rule that
blocks 9021 from the LAN does not interfere — **provided elfldr is actually
running**. If the launch fails with `connect() failed to 127.0.0.1:9021`, or
the dashboard shows the loader as unreachable (`GET /api/elfldr/status`),
elfldr is not running and step 0 below is required. (The user drives this UI;
we never POST to it.)

**Step 0, once per boot — start elfldr:** add `elfldr-ps5.elf` as the first
entry of the autoloader's `autoload.txt` (Y2JB / BD-JB / Lua autoloader, e.g.
[itsPLK/ps5-y2jb-autoloader](https://github.com/itsPLK/ps5-y2jb-autoloader)),
or launch it from
[Payload Manager](https://github.com/itsPLK/ps5-payload-manager) (web UI on
8084), which is also on this console's family of tools. elfldr listens on
`INADDR_ANY:9021` (`elfldr/socksrv.c:329`), kills a previously running
`elfldr.elf`, and is the loader every tool in the ecosystem streams to.

**Then, for us:** `platform/ps5/scripts/deploy.sh platform/ps5/hello/hello.elf`
(`socat - TCP:$PS5_HOST:9021 < payload.elf`). The payload is raw ELF on the
wire; elfldr also accepts a text URI as the whole stream (`file:/data/x.elf`,
`https://...`). elfldr `dup2`s the socket onto the payload's stdio, so the
payload's `printf` output comes back on the same connection.

**Do not:** send anything to 7070, 9046, 9295 or 41800; POST to ProsperoMgr's
API; or assume 9020 exists (it is the exploit stage's loader and is closed
here).

*Superseded for our own deploys (Task A3, section 7):* `deploy.sh
PS5_DEPLOY_METHOD=prospero` now uses exactly two well-formed ProsperoMgr
requests — `POST /api/payloads/upload?name=<file>` and `POST
/api/payloads/launch` — after GET-only status checks. Everything else in this
paragraph still holds: no PKG, settings, delete, rename or shutdown routes, and
no raw bytes to 7070.

### 6.7 Live console log on this setup

In order of preference:

1. **etaHEN klog, port 9081** — no payload needed, but it must be enabled.
   etaHEN's Toolbox toggle is "Klog — Klog Server on Port 9081"
   (`Source Code/shellui/assets/etaHEN_toolbox.xml`), stored as
   `Klog=` under `[Settings]` in `/data/etaHEN/config.ini`, **default 0**
   (`Source Code/shellui/src/MonoUtils.cpp`, `Source Code/README.md` line 122).
   Then `PS5_KLOG_PORT=9081 platform/ps5/scripts/klog.sh` (its default), or
   `nc $PS5_HOST 9081`. It is closed right now, so klog is off (or etaHEN is
   not loaded — its FTP 1337, DPI 9090 and DPI v2 12800 are all closed too,
   and those default to 0 as well, so "etaHEN running with defaults" and
   "etaHEN absent" look the same from the LAN).
2. **klogsrv, port 3232** — ProsperoMgr already ships `klogsrv.elf` and tails
   3232 for its Logs page, so once the payload is launched from that UI the
   kernel log is visible there with no extra tooling; otherwise
   `platform/ps5/scripts/klog.sh` (defaults to 9081; `PS5_KLOG_PORT=3232` for
   the klogsrv payload). Both are raw `/dev/klog` text with no framing; klogsrv
   sends every line to every connected client.
3. **Payload stdout over elfldr (9021)** — no klog at all, best while
   iterating our own payload: `deploy.sh` keeps the socket open and prints
   whatever the payload writes. A payload that should also appear in 1 or 2
   must call the SDK's `klog_printf` (`<ps5/klog.h>`), as the hello payload
   already does.

### 6.8 What the user has to do

1. Nothing about 7070/9046 — they are ProsperoMgr (dashboard + PKG upload
   socket), never a payload channel. Same for 8082 (GarlicMgr), 9295 (Sony
   Remote Play) and 41800 (unknown, not homebrew).
2. Make one ELF loader exist: autoload `elfldr-ps5.elf` (9021). Leaving 9021
   blocked on the LAN is fine if payload launches go through ProsperoMgr's
   UI, which uses loopback; only the PC-side `deploy.sh` path needs 9021
   reachable from the Mac.
3. Pick a log stream: enable etaHEN's **Klog** toggle (9081), or launch the
   bundled **klogsrv** from ProsperoMgr (3232) and read it in that UI or with
   `klog.sh`.
4. Nothing else needs starting for now: `kstuff`, ShadowMountPlus (API 10101),
   `ftpsrv` and `shsrv` are not listening either — start only what the task
   needs.

Artifacts for this section: `runs/A2/root.html`, `root.headers`,
`resp_api_users.json`, `resp_api_settings_cloud.json`,
`resp_api_cloud_status.json`, `port_probe.txt`.
Cloned sources: `ref/garlic-savemgr`, `ref/ProsperoMgr`, `ref/etaHEN`,
`ref/shsrv`, `ref/ps5-payload-manager`, `ref/ps5-payload-loader`.

## 7. ProsperoMgr HTTP API: upload and launch (from source, Task A3, 2026-10-26)

Source: `ref/ProsperoMgr` at `9dd11c5` ("v1.1 update"), read only. Files that
matter: `source/backend/include/config.hpp`,
`source/backend/src/api_routes.cpp`, `source/backend/src/elfldr_client.cpp`,
`source/backend/src/payload_registry.cpp`, `source/backend/src/http_server.cpp`,
`source/backend/src/klog_client.cpp`, `source/frontend/src/lib/api.js`.
Everything below is from those files, not from probing. The API is served by
libmicrohttpd on `Config::kHttpPort = 7070` (`http_server.cpp`,
`MHD_OPTION_CONNECTION_LIMIT 24`, 60 s timeout).

### 7.1 Endpoints

| Method + path | Purpose | Request | Success body |
| --- | --- | --- | --- |
| `GET /api/status` | identify the service | none | `{"name":"Prospero Manager","version":"1.1","author":"maj0r","http_port":7070,"ftp_port":2121,"elfldr_port":9021,"uptime_seconds":N,"instance_id":"<hex>","privileged":true}` |
| `GET /api/payloads` | list payloads + per-payload state | none | `{"payloads":[{"name","path","size_bytes","mtime_unix","bundled","enabled","favorite","launch_count","last_launched_unix","installed_version","source","source_url"}, ...]}` |
| `POST /api/payloads/upload?name=<file.elf>` | store an ELF in the managed payload dir | **raw file bytes as the whole body**, `Content-Type: application/octet-stream`; the name is a *query* parameter, not multipart | `{"ok":true,"name":"<file.elf>"}` |
| `POST /api/payloads/launch` | stream a stored payload to elfldr | JSON `{"name":"<file.elf>"}` | `{"ok":true,"launched":"<file.elf>"}` |
| `GET /api/elfldr/status` | is the internal loader reachable | none | `{"reachable":true\|false,"error":"..."}` |

Errors are `{"error":"<message>"}` with a non-200 status (`HttpResponse::Error`,
default 400; launch/upload/delete failures use 500, rename 409). Routes are
matched on the exact URL without the query string, so the `?name=` in the
upload path is parsed by the handler, not the router.

`upload` details (`api_routes.cpp` `UploadRoute(... tmp_dir, 512 * kMiB, ...)`,
`payload_registry.cpp` `ImportFile`):

- `name` must pass `IsSafePayloadFilename`: non-empty, no `/`, no `..`, and the
  extension must be `.elf`, `.bin` or `.self` (case-insensitive). Otherwise the
  body is discarded and the answer is `{"error":"invalid payload name (...)"}`.
- The body is streamed to a temp file under `cfg.tmp_dir`
  (`/data/ProsperoMgr/tmp`) by `http_server.cpp`, capped at 512 MiB (413 above
  that), then copied to `<payloads_dir>/<name>.uploading` and renamed into
  place. An existing file of that name is overwritten.
- `source` is recorded as `uploaded` (`/data/ProsperoMgr/payloads/.sources.txt`).
- `curl --data-binary @hello.elf -H 'Content-Type: application/octet-stream'
  -X POST "http://$PS5_HOST:7070/api/payloads/upload?name=hello.elf"` is exactly
  what the frontend does (`uploadWithProgress` in
  `source/frontend/src/lib/api.js` sends a single octet-stream body).

`launch` details (`payload_registry.cpp` `PayloadRegistry::Launch`):

1. `Resolve(name)` — exact filename match against `List()`, which covers both
   `<root>/payloads` and `<root>/bundled`; unknown names get 500
   `payload not found: <name>`.
2. Refuses names listed in `<payloads_dir>/.disabled.txt`
   (`<name> is disabled — enable it first`).
3. `ElfldrClient::LaunchFile` reads the file, `connect()`s to
   `127.0.0.1:9021` (compile-time constants `kElfldrHost`/`kElfldrPort`,
   `config.hpp`), sends the bytes, then does a 250 ms `MSG_PEEK` and treats a
   reply starting with `[elfldr.elf]` as a rejection; anything else is the
   payload's own output and is discarded when the socket closes. So a launch
   through ProsperoMgr **never returns the payload's stdout** — only lines the
   payload sends to the kernel log are observable, on the console's klog stream.
4. Bumps `.launches.txt` (`launch_count`, `last_launched_unix`) on success.

`elfldr/status` is just `ElfldrClient::Reachable`: socket + `connect()` +
`close()` to `127.0.0.1:9021`. `reachable:true` proves an elfldr is listening;
it does not prove the next ELF will run.

Routes that exist and that we deliberately never script: `POST
/api/payloads/delete|enable|favorite|rename`, `POST /api/payloads/import-path`,
`GET /api/payloads/download*`, `POST /api/payloads/...` nothing else, the PKG
routes (`/api/pkg/*`, 9046 WebSocket), `/api/ftp/*`, `/api/autoloader/*`,
`/api/paths/payloads-dir`, `/api/shutdown`. The whole API is unauthenticated.

### 7.2 Log sources differ by port

ProsperoMgr's own Logs tab (`GET /api/klogs` → `{"connected":bool,"lines":[...]}`,
`GET /api/logs` for its own `prosperomgr.log`) tails **127.0.0.1:3232**
(`Config::kKlogsrvPort`), i.e. the bundled `klogsrv` payload. It does not know
about etaHEN's forwarder on 9081, so "ProsperoMgr shows no klog lines" does not
mean the payload logged nothing. Our portable capture is
`platform/ps5/scripts/klog.sh`, which defaults to 9081 and accepts
`PS5_KLOG_PORT=3232`.

### 7.3 Live check of this console (Task A3)

GET-only calls against 7070 (`runs/A3/status.json`, `elfldr-status.json`,
`payloads.json`, `processes.json`):

- `/api/status`: `Prospero Manager` 1.1, `http_port` 7070, `elfldr_port` 9021,
  `privileged:true`, uptime ≈ 45 min.
- `/api/elfldr/status`: `{"reachable":true,"error":""}` — elfldr is running on
  the console's loopback, so the ProsperoMgr launch path is usable.
- `/api/payloads`: 5 entries — bundled `ftpsrv.elf`, `klogsrv.elf`; uploaded
  `nanodns.elf`, `webkit-autoloader-installer_v0.6.1.elf`, `orbit_store.elf`.
- `/api/processes`: `elfldr.elf` (pid 87), `ProsperoMgr.elf` (93),
  `shadowmountplus.elf`, `kstuff.elf`, `nanodns.elf`, `payload.elf`; **no
  etaHEN process**.
- Port 9081: connection refused, twice, several minutes apart. Consistent with
  the process list: etaHEN is not loaded, so its klog server cannot be on.

Consequence for the task's order (status → start klog → upload/launch hello):
the loader half is ready, the log half is not. etaHEN's Klog toggle starts the
server immediately at runtime (`etaHEN Source Code/util/source/msg.cpp`,
`BREW_UTIL_TOGGLE_KLOG` → `start_klog()`, "Klog Server Started … Port: 9081"), so
no console reboot is needed; etaHEN itself must be running first. Until 9081 is
open, a payload launched through ProsperoMgr leaves no observable output on this
setup (ProsperoMgr drops the elfldr socket, and 3232 is closed without klogsrv).

### 7.4 What the user has to do

1. Load etaHEN (any of the usual autoloaders / payload channels), then turn on
   etaHEN Toolbox → **Klog** ("Klog Server on Port 9081"); the server starts
   when the toggle flips and `Klog=1` is written to `/data/etaHEN/config.ini`.
   Check with `nc -z $PS5_HOST 9081` or `platform/ps5/scripts/klog.sh`.
2. Leave ProsperoMgr running (7070) — it is already there and its loopback
   elfldr is up, so nothing else needs starting.
3. Then: `platform/ps5/scripts/klog.sh "$SPLAT_DATA_DIR/runs/ps5-hello.log"`
   in one terminal and `PS5_DEPLOY_METHOD=prospero
   platform/ps5/scripts/deploy.sh platform/ps5/hello/hello.elf` in another.
   The hello payload writes every line with `klog_puts()` as well as stdout, so
   its lines appear in the captured klog.

## 8. Live log and payload loop working (Task A4, 2026-10-26)

Supersedes the *defaults* of 6.7 and the *user actions* of 7.4: the log source
is now **klogsrv on 3232**, started by our own scripts through ProsperoMgr's
API, with etaHEN still not loaded. Console addresses stay out of the repo
(`$PS5_HOST` from `.env.local`).

### 8.1 klogsrv launched through ProsperoMgr (no UI, no LAN 9021)

Starting state: 3232 refused, 7070 answered as ProsperoMgr 1.1
(`/api/status`), `/api/elfldr/status` → `{"reachable":true,"error":""}`, so the
loopback loader was already there. Two well-formed requests were enough:

```
GET  /api/payloads                                -> klogsrv.elf, bundled:true, enabled:true
POST /api/payloads/launch {"name":"klogsrv.elf"}  -> {"ok":true,"launched":"klogsrv.elf"}
```

3232 accepted a TCP connection within ~2 s, `/api/processes` lists `klogsrv.elf`
(pid 100 this session) and `/api/payloads` shows `launch_count: 1` for it. The
launched file is ProsperoMgr's own bundled copy,
`/data/ProsperoMgr/bundled/klogsrv.elf` (119752 bytes, upstream
ps5-payload-dev/klogsrv, unmodified), so **nothing had to be uploaded and no
klogsrv build was needed**. `$SPLAT_DATA_DIR/ref/klogsrv` (which already holds a
built `klogsrv-ps5.elf`) stays the source for our own copy if a console's
ProsperoMgr lacks the bundled one.

9021 stayed refused from the Mac the whole time: ProsperoMgr does the launch
over its own `127.0.0.1:9021`, exactly as section 7 describes.

### 8.2 What a captured klog looks like

`platform/ps5/scripts/klog.sh` (3232) during a `deploy.sh hello.elf`, raw lines
(`runs/ps5-hello.log`, `runs/console/*-hello.log`):

```
<118>[elfldr.elf] socksrv.c:246:recv: No such file or directory
<118>[ProsperoMgr] POST /api/payloads/upload name=hello.elf -> 200 (32ms)
<118>[payload.elf] hello: start (ps5-splat-vr)
<118>[payload.elf] hello: firmware 13.60 (kern.sdk_version 0x13600007)
<118>[payload.elf] hello: tick 1/5
<118>[ProsperoMgr] POST /api/payloads/launch name=hello.elf -> 200 (331ms)
...
<118>[payload.elf] hello: done, exit 0
# process pid=101, payload.elf calls exit() exit_value=0.
```

Two things to note: a payload launched this way is named **`payload.elf`** by
the loader (its `klog_puts` prefix is `[payload.elf]`, *not* the uploaded file
name), and ProsperoMgr writes its own `POST /api/payloads/...` bookkeeping lines
into the kernel log — those contain `name=hello.elf` and must not be mistaken
for the payload's output. The launch takes ~330 ms end to end; the kernel log is
otherwise full of `SceShellCore` FMEM/VM-Stats chatter every second.

Because every launch shares that process name, `[payload.elf]` alone does not
identify *which* payload wrote a line; match the message text. A `payload.elf`
process (pid 94) was already running before this task (A3 saw one as well) — not
ours, not klogsrv, left alone.

`kern.sdk_version` reports `0x13600007` now (A2/A3 saw `0x13600000`); both are
13.60, so do not compare the low byte for the firmware version.

### 8.3 klogsrv behaviour the scripts rely on

- One `/dev/klog` reader **per client connection**, starting at the current end
  of the log: nothing is replayed, so connect *before* launching a payload.
- The accept loop is endless, so the process survives ProsperoMgr closing the
  elfldr socket after the launch (its banner `puts()` just goes nowhere). On a
  relaunch it kills an earlier `klogsrv.elf` first (`find_pid` + `SIGKILL`).
- Binds `INADDR_ANY:3232` (override with `-p PORT`), writes raw kernel text to
  every client, no framing, and only while at least one client is connected.
- Section 7 stays true for ProsperoMgr's own Logs tab: it tails the same loopback
  3232 and never starts klogsrv itself, and bundled autostart is off
  (`/data/ProsperoMgr/bundled_autostart.txt` absent).

### 8.4 Scripts that now do it by themselves

- `platform/ps5/scripts/klog.sh [--ensure] [FILE]` — default `PS5_KLOG_PORT=3232`;
  when the port is closed it checks `GET /api/status` (must be ProsperoMgr) and
  `GET /api/payloads` (klogsrv present and enabled), then
  `POST /api/payloads/launch` and polls 3232 for up to 10 s. Prints
  `klog: streaming <host>:<port>` once connected. `--ensure` stops there.
  Port 9081 (etaHEN) can still be read with `PS5_KLOG_PORT=9081`, but the script
  never starts etaHEN.
- `platform/ps5/scripts/run.sh <elf> [seconds]` — the dev loop: ensure the log,
  connect, `deploy.sh`, capture N seconds (default 12) to stdout and to
  `${SPLAT_DATA_DIR:-<repo parent>}/runs/console/<UTC stamp>-<name>.log`. Exits
  0 when the payload's start line appeared, 1 when it did not, 2 usage, 3 no log
  stream, 4 deploy failed, 5 the log stayed empty. "Start line" = first log line
  naming the payload (ELF stem, case-insensitive; `PS5_RUN_READY` overrides),
  with ProsperoMgr's `api/payloads` lines ignored.
- `platform/ps5/scripts/deploy.sh` is unchanged in behaviour (still the four
  calls of section 7) and now shares `ps5_http`/`ps5_json_field` from
  `ps5-env.sh`; a refused connection prints one line instead of a traceback.

### 8.5 What the user has to do

Nothing, as long as ProsperoMgr keeps running. After a console reboot klogsrv is
gone and 3232 closed again, and the next `klog.sh`/`run.sh` starts it — no UI
step, no 9021 opening, no etaHEN. What is on the console: see the table in
`platform/ps5/README.md` (bundled `klogsrv.elf`, uploaded `hello.elf`); nothing
else was added, deleted or changed.

Artifacts: `runs/ps5-hello.log`, `runs/console/*-hello.log`, `runs/A4/`
(`launch-klogsrv.txt`, `console-state.txt`, `klog-step3.*`, `run*.std{out,err}`,
`mock_prospero.py` + mock logs for the offline exit-code tests).

## Sources

- ps5-payload SDK: <https://github.com/ps5-payload-dev/sdk> (README, v0.45 release)
- elfldr: <https://github.com/ps5-payload-dev/elfldr>
- ftpsrv: <https://github.com/ps5-payload-dev/ftpsrv>
- klogsrv: <https://github.com/ps5-payload-dev/klogsrv>
- websrv: <https://github.com/ps5-payload-dev/websrv>
- etaHEN: <https://github.com/etaHEN/etaHEN>
- GarlicMgr / garlic-savemgr: <https://github.com/earthonion/garlic-savemgr>
- ProsperoMgr (7070, 9046): <https://github.com/notmaj0r/ProsperoMgr>
- PS5 payload and port table (community): <https://psxtools.de/forum/index.php?thread/92971-ps5-payload-port-guide/>
- PS5 Payload Manager (8084, elfldr on 9021): <https://github.com/itsPLK/ps5-payload-manager>
- Y2JB autoloader (autoload.txt): <https://github.com/itsPLK/ps5-y2jb-autoloader>
- Remote Play ports: <https://remoteplay.dl.playstation.net/remoteplay/lang/en/1100006.html>
- PS5LM: <https://github.com/cobanov/PS5LM> (`docs/BUILDING.md`, `docs/CONSOLE.md`, `scripts/`)
- PS5LM Plus: <https://github.com/abhi245y/PS5LM-Plus>
- PS5VR: <https://github.com/mtp-24/PS5VR> (`app/src/app_vr.c`, `docs/firmware-support.md`)
- PS5_Vulkan: <https://github.com/mihawk-99/PS5_Vulkan> (`tools/build-radv.sh`)
- PS5_Mesa: <https://github.com/mihawk-99/PS5_Mesa>
