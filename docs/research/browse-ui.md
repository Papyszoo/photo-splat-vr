# Browse/gallery UI: console photos and splat albums

Task UI1, 2026-10-26. Research and design only; nothing was built, and the only console traffic was a **read-only directory listing** (ProsperoMgr `GET /api/ftp/list`): no mkdir, upload, delete, launch or file download. Raw replies in `$SPLAT_DATA_DIR/runs/ui_browse/`; the console address is not recorded here.

Goal (user): "a UI so we can browse the console for photos, and all processed splats can be grouped (folders possibly) and selected to display."

## 1. Where the photos are (probe, 2026-10-26)

### 1.1 Console captures

```
/user/av_contents/photo/NPXS40087/CUSA12392/2c4/
  20261025_052805_00338445.jxr    157,237 B   HDR screenshot
  20261025_052805_00338445.meta       226 B   companion metadata (not read)
  20261025_052805_00338445.dat      1,024 B
  20261025_052805_00338445.ext         70 B
/user/av_contents/thumbnails/photo/NPXS40087/CUSA12392/2c4/
  20261025_052805_00338445.jxr.jxr 121,912 B   thumbnail
```

- **One capture in total** on this console (~280 KB with its thumbnail). The layout is `photo/<NPXS40087>/<TITLE_ID>/<hash>/<YYYYMMDD_HHMMSS>_<8 hex>.<ext>`, mirrored under `thumbnails/photo/`; `NPXS40087` is the capture library's service id, the per-title folder is the game that was running.
- `.jxr` is JPEG XR: the PS5 writes it when screenshot HDR is on. With HDR off the user chooses jpg or png (Settings > Captures and Broadcasts). HDR is on here, so nothing in this folder decodes with `stb_image`.
- `/user/av_contents/video/` mirrors the layout for clips (out of scope); `sdr/`, `extra/`, `content_tmp/` held no photos.

### 1.2 Other candidate roots

| Root | Contents now | Use |
| --- | --- | --- |
| `/mnt/usb0..7` | usb0: packages, homebrew, one `.pkg.part`; usb1-7 empty | **USB photos** (the export route) |
| `/mnt/ext0`, `/mnt/ext1` | ext1 = extended storage (`user/`, `system_data/`), no `av_contents` | M.2 photos, if any |
| `/user/download` | installs incl. our `PPSA9920x` tests | not a photo source |
| `/data/ps5-splat-vr` | `scenes/` (5), `models/` (2 GGUF), `bench/`; no `inbox/` yet | our data, LAN inbox |

There are **no user photos on USB or in the inbox today**; the only "console photo" is that one HDR capture. A v1 that browses only captures shows one item, so the useful sources are the LAN inbox, USB, and captures (exported to USB as JPG, or through the helper payload of §1.4).

### 1.3 Can a title read them?

- A title sees `/app0`, its own `/download0`, and `/data` + USB once the HEN opens them: PS5LM writes `{"PID":n}` to `/download0/etahen_jailbreak`, which needs the title in `app_jailbreak.exact_title_ids` (PS5LM `CHANGELOG.md:16-19`); it enumerates `/mnt/ext0` and `/mnt/usb0..3` (`CHANGELOG.md:177`). HLA-PSVR2 states the same boundary: "`/data` and USB come from a per-title helper (Lapy)" (`docs/research/hla-psvr2.md:93`).
- Our viewer already **reads** `/data/ps5-splat-vr/scenes` from the title (`docs/console-runbook.md:156`). **Writing** `/data` from a title is still unproven (`console-runbook.md:83`); the HEN allowlist is the known fix.
- **Reading `/user/av_contents` from a title: answered by the T7 spike (2026-10-26) — a title cannot see it.** The viewer's one-run probe (`PROBE=sources` in `viewer.env`, `platform/ps5/viewer/main.cpp`) ran in the `<TITLE_ID>` title process, TV mode, 30 s, and logged `opendir`/`errno` for each root (recurse 3 levels under the photo folder, 200 entries per root) plus a 16-byte read of the first image found. Results:

  | Path (from inside the title) | `opendir` | Entries | Host-side view (ProsperoMgr `GET /api/ftp/list`) |
  | --- | --- | --- | --- |
  | `/user/av_contents/photo` | errno 2 (ENOENT) | — | 1 dir (`NPXS40087/...`, the one HDR capture) |
  | `/user/av_contents` | errno 2 (ENOENT) | — | 6 dirs (`photo`, `video`, `thumbnails`, `sdr`, `extra`, `content_tmp`) |
  | `/mnt/usb0` .. `/mnt/usb7` | errno 2 (ENOENT) | — | `/mnt` lists `usb0`; the stick is out today, `usb0` is empty |
  | `/mnt/ext0` | errno 2 (ENOENT) | — | `/mnt` has an `ext0` directory |
  | `/mnt/ext1` | ok | **3** | 3 (`homebrew`, `user`, `system_data`) — identical |
  | `/data/ps5-splat-vr/inbox` | errno 2 (ENOENT) | — | absent there too (4 entries: `models`, `bench`, `scenes`, `title-quit`) |

  ENOENT, not EACCES: those paths are not in the title's namespace at all, while `/mnt/ext1` lists exactly what the host sees and `/data` is readable (the same run loaded 11 scene folders). No image file was visible on any reachable root, so the 16-byte magic read was not exercised — there is no source root a title could read a capture from today. Consequence for T2: **inside the title the only photo root left is `/data` (our inbox)** — captures fail and USB failed in that run (no stick was mounted at the time; see §1.4 B for how the alias works), so a v1 that browses captures or a USB stick needs either the host/LAN relay or the HEN's per-title allowlist to expose `/user/av_contents` and `/mnt/usb*`; the spike is the measurement, not a settled answer on that (routes compared in §1.4). Raw log in `$SPLAT_DATA_DIR/runs/t7_spike/`.

  Re-run once more with the extended probe (P1, 2026-10-26 18:09 local, same title, TV, 30 s, `PROBE=sources`): same answer, and now `/user` itself is ENOENT too, so the whole tree is outside the namespace, not only `av_contents`. The ShadowMountPlus alias roots were added in that run; the per-volume paths, the results table and what is left for the user are in §1.4 B and §1.5. Raw log `$SPLAT_DATA_DIR/runs/p1_probe/`.

### 1.4 Getting photos into the app

Four routes, checked in the reference clones and on the web (2026-10-26); only B and A have since been probed on the console (read-only, §1.4 B and A below), so the T7 numbers in §1.3 remain the only title-side measurement of captures.

**A. The title unjails itself through the HEN.** A title has no kernel R/W of its own: the SDK's kernel calls need the `payload_args` handoff (`ref/PS5_PayloadSDK/include/ps5/payload.h:25-32`, `ref/PS5_PayloadSDK/crt/kernel.c:201-205`); the 13.60 kernel offsets exist (`ref/PS5_PayloadSDK/crt/kernel.c:481-489`) but only a payload is given the pipes. etaHEN instead watches for `{"PID":n}` in `/mnt/sandbox/<TID>_*/download0/etahen_jailbreak` (`ref/etaHEN/Source Code/daemon/source/commands.cpp:662-739`), and `Hijacker::jailbreak(true)` sets uid 0, authid `0x4801…0013`, caps −1, attr `0x80`, `fd_rdir`/`fd_jdir` = rootvnode (`ref/etaHEN/Source Code/libhijacker/source/hijacker.cpp:63-100`) — the **real root**, so `/user/av_contents` resolves. **Blocker:** that daemon (local 2.5B source) serves a hard-coded whitelist (ITEM00001, NPXS39041, DUMP00000, PKGI13337, PKGI12345, TOOL00001, `*LAPY*`, same `commands.cpp:377-398`); there is no config key and the title's id is not on it. OnionHEN makes the list configurable (`app_jailbreak.exact_title_ids`, max 20, `title_id_prefixes`; [config.ini.example](https://github.com/aydencharles/onionHEN/blob/main/config.ini.example)). Precedents: PS5LM (`ps5/app/main.cpp:257-277`), PS5VR (`PS5VR/app/src/eng_jailbreak.h:10-16`), Xplorer, Kylin Explorer ("etaHEN-compatible sandbox trigger"). Effort **S** (~100 LOC + one HEN config line); risk **M/H**: the app then runs as root with the sandbox open, the mid-run credential swap breaks later module loads (`PS5VR/app/src/eng_jailbreak.h:7-16`, `PS5VR/app/engine/media/include/eng_vdec.h:72-75`), and every `LoadExec` restart is sandboxed again. **Measured on this console (2026-10-26, read-only): there is no HEN at all.** `GET /api/processes` lists kstuff.elf, nanodns.elf, shadowmountplus.elf, ProsperoMgr.elf, klogsrv.elf, elfldr.elf and an unnamed `payload.elf` — no etaHEN and no OnionHEN; neither `/data/etaHEN` nor `/data/OnionHEN` exists; etaHEN's own klog forwarder (9081) and FTP (1337) are closed; and `/data/ps5_autoloader/autoload.txt` is kstuff → nanodns → shadowmountplus → ProsperoMgr → garlic-savemgr. So A is not a config edit away: the user has to load OnionHEN first, and the `<TITLE_ID>` sandbox measured in §1.3/§1.5 is the HEN-less default.

**B. ShadowMountPlus's existing mounts (no config change).** `/data` is a writable nullfs for PPSA titles, and **mounted** volumes `/mnt/usb0..7`, `/mnt/ext0..1`, `/mnt/disc` get per-volume aliases (`ShadowMountPlus/README.md:206`, `ShadowMountPlus/src/sm_fakelib.c:533-585`); no config exists for extra paths (`ShadowMountPlus/config.ini.example`: scanpath/emulators/fakelib only), so av_contents cannot be bound into the title without forking SMP (**L**, and it changes the user's payload — excluded).

The exact paths SMP creates are `<sandbox>/mnt/usb0..7`, `<sandbox>/mnt/ext0..1`, `<sandbox>/mnt/disc`, plus `<sandbox>/data` and `<sandbox>/preinst/common/font`: `mount_sandbox_directory()` builds `mount_path = <app0 minus "/app0"> + <source path>` from `mount_sandbox_storage_roots()` (`src/sm_fakelib.c:362-373, 533-591`; the root list is the 8 USB + 2 ext + `/mnt/disc` slots, `SANDBOX_USB_ROOT_COUNT`/`SANDBOX_EXT_ROOT_COUNT` at `:25-26`). Inside the title `<sandbox>` is `/`, so those aliases are the title's `/mnt/usbN`; SMP's own log spells the pair out, e.g. `[FAKELIB] directory mounted for <TITLE_ID>: /mnt/usb0 -> /mnt/sandbox/<TITLE_ID>_000/mnt/usb0`.

Measured on the console, read-only, 2026-10-26 (no stick in the console now):

- A 250 GB exFAT stick **was** in `/mnt/usb0` from boot (11:26) until `[SCAN] USB storage disconnected: /mnt/usb0` at 17:26:12 (`/data/shadowmount/debug.log`), and every title launched in that window got the `usb0` alias, ours included (`[FAKELIB] … mounted … /mnt/sandbox/<TITLE_ID>_000/mnt/usb0`, 15:20 and 16:31). It was pulled before the 17:47 T7 run, which is why that run saw no `usb0`; empty mount directories get no alias at all, hence ENOENT rather than an empty listing.
- In the P1 run (18:08 launch) the title's `/mnt` lists exactly **1** entry — 18 directories on the host — and SMP's log for that launch shows `/mnt/ext1` as the only volume alias it created, so that entry is the `ext1` alias; `/app0/../mnt` lists the same. `/app0/..` itself lists (9 entries, the sandbox root), which confirms `/` inside the title is `<sandbox>`. So the sandbox `/mnt` is SMP's local alias directory, and a title can enumerate which volumes are mounted by listing `/mnt` instead of guessing `usb0..7`.
- `/mnt/ext1` itself lists (10 entries), but one of its subfolders fails with **errno 13 (EACCES)**, not ENOENT: the alias grants the volume root, not every subtree.

So the mechanism is confirmed from both sides for a mounted volume, and the T7 `/mnt/usb*` ENOENT is fully explained; what is still unmeasured is a **read from inside the title of a USB volume**. Effort **S** to finish, risk low — the probe is in place and needs a stick in the console (`PROBE=sources`, `docs/console-runbook.md` §1b). `/mnt/disc` stays untested for the same reason.

**C. Companion payload (recommended for captures and USB).** A payload sees the whole filesystem, and its 0 B direct pool is irrelevant to file I/O (`docs/research/spike-b-memory.md` §1). Precedents: `ps5vr-helper` hands its title the real root over loopback 9079, on 13.60 (`PS5VR/helper/ps5vr-helper.c:89-105`, `PS5VR/README.md:73`); ps5-media-gallery (:3737, [dekazeta](https://www.dekazeta.net/foro/files/file/5507-ps5-media-gallery/)) and airpsx ([`/api/media/list`](https://deepwiki.com/barisyild/airpsx/6.4-media-endpoints)) already scan `/user/av_contents`; ps5-exporter (:9100) is the small-payload-with-HTTP pattern. The title can start it: push the bundled ELF to elfldr `127.0.0.1:9021` (raw ELF, no auth, `ref/elfldr/socksrv.c:36-46,404`) — loopback out of a title is proven by PS5VR's 9079 call — or the user's autoloader keeps it resident. Shape: `list`/`fetch` on a loopback port over captures + `/mnt/usb*`, or copy the picks into `/data/ps5-splat-vr/inbox` so the normal pipeline sees plain files and the title stays sandboxed. Effort **M** (2-4 d); risk **M**: re-arm after reboot/rest mode, firmware-specific exports, one more root process of ours on the console.

**D. Routes that work today.** ProsperoMgr (a payload, so it sees the whole filesystem) has a server-side copy route that can put a capture straight into the inbox (`ref/ProsperoMgr/source/backend/src/api_routes.cpp:1388` copy, `1565` upload, `1144` list), and etaHEN's FTP (1337) accepts files — manual, needs a browser. The LAN upload page cannot do it on the console: `core/ingest` is not in the PS5 build (§2.3), so adding it is **M**. On the console itself, Media Gallery → copy to USB is reported to write **JPG** ("files you share or transfer will be in a more compatible format, without HDR") and HDR captures lock the format to "Auto" ([seekingtech](https://seekingtech.com/how-to-change-the-screenshot-and-video-formats-on-ps5/); not verified here) — USB + B is then a zero-code path to captures. Effort **S** (docs only), no risk.

**JXR.** HDR captures are 16-bit PQ `.jxr` with a `.jxr.jxr` thumbnail; jxrlib is BSD-2, but its sample decoder rejects PS5 captures ("Unsupported pixel format", [ps5upload #448](https://github.com/phantomptr/ps5upload/issues/448)), so shipping it is decoder + tone-map + tests (**M/L**). Skip it while captures can arrive as JPG (USB) or PNG (SDR: Settings → Captures → Screenshot Format).

| Route | Gets | Effort | Risk | Verdict |
| --- | --- | --- | --- | --- |
| D inbox (FTP / ProsperoMgr / LAN page) | any uploaded photo | S (-M for the page) | none | **v1** |
| B SMP USB alias | USB stick | S | low | mechanism confirmed, one stick-in probe left |
| C helper payload | captures + USB | M | M | **v2, the supported escape** |
| A HEN app jailbreak | captures + USB | S | M/H | only with OnionHEN |
| jxrlib in the title | HDR captures in place | M/L | low | deferred |

**Recommendation:** v1 ships **D** (inbox only, `.jpg/.png`, LAN page added to the PS5 build); v2 adds **C** as the single supported route to captures and USB, because it needs no HEN config, no SMP fork and leaves the title sandboxed; **A** is the cheap alternative if the user moves to OnionHEN; **B** is one USB probe; jxrlib waits.

### 1.5 Formats and probe method

`jpg/jpeg/png` only (`core/jobs/inbox_watcher.cpp:20-23`), decoded by `stb_image` (`core/jobs/image_io.cpp:48-76`); thumbs use `stb_image_resize2` (`image_io.cpp:92-109`). No JXR, no HEIC. The probe now matches `.jpg/.jpeg/.png` case-insensitively (`.jxr` kept so an HDR capture would still be spotted) and walks each storage-alias root one level down, inside the same read-only bounds as before (3 levels under `/user/av_contents/photo`, 200 entries per root, errno or 4-byte magic only). The brief's `POST /api/ftp/list {"path":…}` returns 404 on this ProsperoMgr 1.1 build; the route is `GET /api/ftp/list?path=…` (`ref/ProsperoMgr/source/backend/src/api_routes.cpp:1144`), so GET was used.

Host-side reads for the P1 session (all GET: nothing written, no payload launched, no elfldr connection): `/api/ftp/list?path=` (directories), `/api/ftp/storage` (mounted volumes — how "is a USB mounted right now" was answered), `/api/ftp/text?path=` (small text files: viewer.env, `/data/ps5_autoloader/autoload.txt`, `/data/shadowmount/debug.log`; `:1460`), `/api/processes` (`:1710`), `/api/titles/running`, `/api/klogs` (`:529`, the live 500-line kernel ring).

Inside the title (P1 run, 2026-10-26 18:09 local, TV, 30 s, `immichvr-1`, `PROBE=sources`, viewer `PASS, exit 0`):

| Path | Title | Host |
| --- | --- | --- |
| `/user` | errno 2 | exists |
| `/user/av_contents` | errno 2 | 6 dirs |
| `/user/av_contents/photo` | errno 2 | 1 dir |
| `/mnt` | ok, **1** entry | 18 dirs |
| `/mnt/usb0` .. `/mnt/usb7` | errno 2 (×8) | present, empty (no stick) |
| `/mnt/ext0`, `/mnt/disc` | errno 2, errno 2 | present, empty |
| `/mnt/ext1` | ok, 10 entries, one subfolder errno **13** | 3 dirs |
| `/app0/..` | ok, 9 entries | — (the sandbox root) |
| `/app0/../mnt` | ok, 1 entry | — (the same alias set) |
| `/data` | ok, 16 entries | 16 |
| `/data/ps5-splat-vr` | ok, 4 entries | 4 |
| `/data/ps5-splat-vr/inbox` | errno 2 | absent |

**Settled:** `/user/av_contents` is outside the title's namespace, whole tree (§1.3); the SMP alias set is real for a mounted volume and enumerable from the title by listing `/mnt` (§1.4 B); `/data` reads exactly as the host sees it; the ext1 alias is root-only, one level down hits EACCES. **Still needs the user:** a USB stick in the console with a `.jpg`/`.png` on it and one more `PROBE=sources` run — that decides the USB source for T2 and exercises the 16-byte read (nothing reachable holds an image today); and OnionHEN must be loaded before route A can be tried at all, since no HEN runs here (§1.4 A). Raw files: `$SPLAT_DATA_DIR/runs/p1_probe/` (klog capture, host listings, SMP log, viewer.env before/after).

## 2. What exists today

### 2.1 Scene store (C5)

- One folder per photo: `original.jpg`, `thumb.jpg`, `scene.psplat`, `meta.json` (`core/scene/scene_folder.h:38-45`, `docs/spec.md:136-141`).
- `meta.json`: `format_version`, `backend`, `intrinsics`, `seconds_taken`, `splat_count` (`scene_folder.h:25-31`, `scene_folder.cpp:27-40`). **No capture date, original name, source or album.** The writer insists on non-empty original and thumbnail JPEGs (`scene_folder.cpp:86-108`).
- On the console all 5 scene folders hold **only `scene.psplat` + `meta.json`** (probe): `push-data.sh` sends just those two (`platform/ps5/scripts/push-data.sh:133-137`), so there are no `thumb.jpg` files to show today. Console folder names are hand-picked, restricted to `[A-Za-z0-9._-]` (`push-data.sh:145-149`); desktop uses the job id (`docs/desktop-app.md:218`).

### 2.2 Playlist and viewer

- `ScenePlaylist` = immediate subdirectories of `SCENES_DIR` holding `scene.psplat`, sorted by folder name; a folder that itself holds `scene.psplat` is a one-entry playlist; next/previous wrap (`core/render/scene_playlist.cpp:9-60`). **Flat, no recursion, no albums.**
- VR: L1/R1 step through it; one scene is preloaded and swapped through the staging slot (`platform/ps5/viewer/vr_mode.cpp:640-678`, `498-560`). TV mode uses the same playlist with a `SCENE=<folder>` override (`platform/ps5/viewer/main.cpp:437-459`); the desktop app re-sorts newest first (`platform/desktop/app_mode.cpp:310-350`). The viewer boots straight into a scene: no menu, no gallery, no text.

### 2.3 Jobs and the LAN page

- `JobQueue`: id, photo, backend, state, scene_folder, seconds, persisted JSON; a job left `running` is re-queued on load (`core/jobs/job_queue.h:19-28`, `job_queue.cpp:138`).
- `JobRunner`: one at a time, writes `output_root/<id>`, builds `original.jpg` + `thumb.jpg` + scene + meta, reports fraction + stage (`job_runner.h:16-21`, `job_runner.cpp:91-104,149-153,183-186`).
- LAN page: PIN login, upload, queue list. It cannot list, delete, rename or browse (`core/ingest/README.md:42-49`; `ingest_server.cpp:149-213`). Jobs, ingest and model code are **not** in the PS5 build (`platform/ps5/ggml/CMakeLists.txt:70-90`).

### 2.4 Missing for folders/groups

1. No album in meta, `Job` or the folder layout; the playlist is flat.
2. No read-only album/scene/photo listing API in core.
3. No thumbnails on console scenes, nothing that displays one.
4. No 2D layer, no font, no focus/navigation model.
5. Sense controllers are not read; only the DualSense (`vr_mode.cpp:130-183`).
6. No conversion in the title; captures and USB are never scanned.

## 3. UI rendering options

### 3.1 What the stack gives us

- RADV, Vulkan 1.3, **dynamic rendering required and enabled** (`core/render/vulkan_context.cpp:129-149`, `269-272`); the renderer ends the frame in the caller's `final_layout` (`splat_renderer.cpp:996-997,1156`).
- No Vulkan loader in the title: every `vk*` the UI calls needs a shim (`platform/ps5/viewer/vk_entrypoints.cpp:1-19`).
- Two outputs from one colour image: Hmd2 eye buffers (stereo SBS copy, `vr_mode.cpp:775-782`) and TV VideoOut (`main.cpp:680-696`). A UI drawn into that image shows on both.
- **No UI toolkit**: `third_party/` is doctest, ggml, glfw, httplib, json, miniz, stb (image only). No ImGui, no font, no truetype. Bitmap-text precedent: the icon generator embeds a 5x7 font (`platform/ps5/title/gen-icon.py:28`, `title/README.md:78`).

### 3.2 Options

| Option | Fit | Cost |
| --- | --- | --- |
| **A. Minimal own panel** (recommended) | one dynamic-rendering pipeline + textured quads; one atlas holds the 8x8 font and 256x256 thumbnail tiles | ~600-900 LOC + 2 shaders; coarse text at 2-3x; no library |
| B. Dear ImGui (MIT) | `imgui_impl_vulkan` supports `UseDynamicRendering` (since 2023-07) and gamepad nav gives d-pad focus for free | ~10 files vendored, descriptor pool, texture updates, `ImGui_ImplVulkan_LoadFunctions` wired to our `resolve()` |
| C. ps5-homebrew-ui (GPL-3.0) | *rejected*: OpenGL next to RADV needs symbol hiding (4,399 collisions) and 0.35 GiB (`ref/PS5LM-Plus/docs/CONSOLE.md:113`); upstream was pulled once (PS5LM `CHANGELOG.md:25-27`) | not Vulkan, high risk |

Licences: ImGui MIT and jxrlib BSD-2 fit GPL-3.0-or-later; font8x8 is public domain. A and B need the same offscreen UI texture and panel quad, so A does not block B.

HLA-PSVR2 is no precedent for UI: it is only the OpenXR runtime, and the screens are Half-Life: Alyx's own Vulkan UI (no toolkit to reuse). The PS5 homebrew UI kit we found (ps5-homebrew-ui, used by PS5LM Plus and ProsperoAI) is OpenGL, not Vulkan — option C.

### 3.3 Where the panel goes

- Draw the UI **after** the splat pass into the same target, with `final_layout = COLOR_ATTACHMENT_OPTIMAL` and a `LOAD_OP_LOAD` pass.
- VR: a **world-locked quad** at head + 1.6 m forward, 1.4 x 0.8 m, recentred by R3; two eye draws with the splats' `eye_center_shift` (`splat_renderer.h:28-33`). Not screen-locked: that is uncomfortable.
- TV: the same UI texture scaled to 1920x1080. One UI code path for both.
- Memory: 1280x720x4 = 3.5 MB + a 2048² atlas = 16 MB, inside the 1 GiB UI reserve (`docs/spec.md:186`).

## 4. Recommended minimal v1

### 4.1 Data model

```
/data/ps5-splat-vr/
  scenes/                      # scene root, unchanged
    <scene>/                   # loose scene = the implicit "All" album
    <album>/<job-id>/{original.jpg,thumb.jpg,scene.psplat,meta.json}
  inbox/                       # LAN uploads (unchanged)
```

- **Album = directory** under `scenes/`; today's loose scenes stay valid and appear as one implicit album. No index, no migration.
- `Job` gains `album` (empty = root); the runner writes `output_root/<album>/<job-id>`. `meta.json` gains optional `captured_at` and `source` (older readers ignore them).
- New read-only `core/gallery/`: `list_albums(root)`, `list_scenes(album_dir)`, `discover_sources()`, `list_photos(source)`; no Vulkan, host-tested.
- Sources in menu order: **Inbox** (`/data/ps5-splat-vr/inbox`), **USB** (`/mnt/usb0..7`), **Captures** (`/user/av_contents/photo`). Formats `.jpg/.jpeg/.png`; `.jxr` only if jxrlib is approved. USB is in the list only while a stick is mounted (SMP aliases mounted volumes per title, §1.4 B — mechanism confirmed, one stick-in run pending; `/mnt` lists the mounted set), and captures only through the companion payload (§1.4 C); v1 shows **Inbox only**.
- Thumbnails: photos via `stb_image` + resize to 256 (`image_io.cpp:48,92`); scenes use `thumb.jpg` when present, else render once at the capture pose and cache `<scene>/thumb.jpg`, else a labelled placeholder.

### 4.2 Flows

1. **Menu** (boot) -> Photos / Albums / Queue.
2. **Photos** -> source -> grid -> Cross toggles -> "Convert N".
3. **Convert** -> album (existing, or "New: <date>") -> enqueue -> Queue.
4. **Queue** -> per-job stage + bar; done -> "View now" / "In album".
5. **Albums** -> album grid -> scene grid -> Cross -> **View** (existing renderer; L1/R1 within the album; Options -> menu).
6. **Back**: Circle on any screen; Options in a scene.

### 4.3 Wireframes

```
MENU                          PHOTOS: Inbox (12)           ALBUM "2026-10-25"
+-------------------------+   +-------------------------+   +-------------------------+
| PHOTO SPLAT VR          |   | [ ] 0528_00338445.jxr   |   | [thumb] [thumb] [thumb] |
| > Photos                |   | [x] 4.jpg  2.1 MB       |   |  immichvr-1  immichvr-2 |
|   Albums                |   | [x] 5.jpg  1.8 MB       |   |  SHARP 12 s  DA2 0.4 s  |
|   Queue (1 running)     |   | [ ] 6.png  3.4 MB       |   | [thumb] [thumb]         |
|                         |   |                         |   |  photo03    ...         |
| Circle back  Cross open |   | Circle back Convert (2) |   | Cross view  Circle back |
+-------------------------+   +-------------------------+   +-------------------------+

QUEUE (modal, no splat)       VIEW overlay (Options)
+-------------------------+   +-------------------------+
| 4.jpg   SHARP           |   | Options: Menu            |
| [######------] 58%      |   | L1/R1 scene              |
| gaussian decoder        |   | R3 recentre  Circle back |
| 5.jpg   queued          |   | (fades after 3 s)        |
+-------------------------+   +-------------------------+
```

### 4.4 Input mapping (same actions on both devices)

| Action | DualSense | Sense (either) |
| --- | --- | --- |
| move focus | d-pad / left stick | left stick; **laser pointer hover** (right controller ray) |
| select / activate | Cross | trigger (R2) or Cross; pointer click = same |
| back / close | Circle | Circle or grip (L1) |
| page / album prev-next | L1 / R1 | L1 / R1 |
| menu (in a scene) | Options | Options/Create tap |
| recentre | R3 | R3 or Options hold (HLA's pattern) |

Sense needs `scePadOpen(user, 3/4, 0)`, `sceVrTracker2RegisterDevice(2/1, h)` (`ref/HLA-PSVR2/ps5xr/runtime/src/psvr2.c:458-473`), state via `scePadVrControllerReadState` (0x68 B layout, `psvr2.c:517-548`), pose via `sceVrTracker2GetResult` with the pad handle (`psvr2.c:683-690,827-870`). `RegisterDevice` is already in our export table (`platform/ps5/hmd2/hmd2_exports.h:58`); the read is an SDK stub, so no new table. D-pad bits are in the button word the viewer already reads (`vr_mode.cpp:106-111`).

### 4.5 Conversion and progress

- One job at a time. The Queue screen shows the runner's stage + fraction over a neutral backdrop; **no splat renders while the model is resident** (the spec's exclusivity, `docs/spec.md:181`); the menu returns when the job ends.
- SHARP timing: the brief's working figure is ~12 s/photo on the console, not yet measured (desktop M4 50-55 s, `docs/PLAN.md:30`; spec target <=60 s), so the UI must show progress, not a spinner.
- Finished scenes appear in their album; the queue survives a restart (`job_queue.h:30-33`).

### 4.6 State and naming

- Albums are read from disk each time a screen opens; no cache to invalidate.
- Selection basket in memory; optional `state/ui.json` remembers last album/sort.
- Album creation without a VR keyboard: "New: 2026-10-25" or an existing album. Free-form renaming goes on the LAN page (phone keyboard), not an on-screen keyboard in v1.

### 4.7 Models screen and release packages

UI1b, 2026-10-26, design only (no console, no download, no code). User decision (`docs/PLAN.md:14`): the release carries **no weights**; a model screen downloads the rest, SHARP only after the user accepts Apple's licence on screen. The C1 manager is reused as built, not redesigned:
- states `license_required -> accepted -> downloading -> verifying -> converting -> ready`, `failed` with a message; one record per backend, revalidated against the GGUF header on every load (`core/model/model_manager.h:26-34,87-93`, `model_manager.cpp:275-351`).
- pinned identity per backend: official file name, URL, size, SHA-256, GGUF architecture, converter version, licence URL/name (`model_manager.h:39-55`), with SHARP and DA2-Small built in (`model_manager.cpp:170-200`; pins in `core/model/checkpoint.h:14-22`, `da2_profile.h:14-30`).
- licence gate: text fetched at runtime and never bundled; the acceptance is the SHA-256 of the text shown, the read fails closed, and `fetch()`/`import()` refuse before it (`model_manager.cpp:234-273,586-601,630-639`, `core/model/license.h:15-43`).
- transfer and convert: HEAD size check, `.part` resume, inbox pickup by exact name, streamed SHA-256, GGUF metadata check, atomic rename (`model_manager.cpp:618-751,471-531`, `core/model/sha256.h`); `status_all()` is the list screen's data (`:403-419`).

Driven today only by the CLI (`tools/model_cli/main.cpp:1-12`) and the desktop backend choice (`platform/desktop/app_mode.cpp:193-236,544-547`). **The LAN page has no model route** (`core/ingest/ingest_server.cpp:149-213`, `core/ingest/README.md:42-49`) and cannot relay a checkpoint: multipart is buffered in RAM under a 40 MB cap (`core/ingest/upload_store.h:18`).

The model screen (new panel, same renderer as §3.3 and the input map of §4.4):

```
MODELS (1 ready)                   LICENCE: SHARP
+------------------------------+   +------------------------------+
| > SHARP       1.22 GB  READY |   | LICENSE_MODEL (ml-sharp)     |
|   Apple research     11.8 s  |   | This license permits use ... |
|   DA2-Small     47 MB  ready |   | ... full text, L1/R1 page ...|
| > MoGe-2 B      420 MB       |   | Cross: accept + download     |
|   MIT          not offered   |   | Circle: back                 |
|   DA3-Small     140 MB       |   +------------------------------+
| converter uses: SHARP        |
| Cross download               |
| Options delete               |
+------------------------------+
SHARP [#########-----] 58%  1.6 / 2.6 GB  (resumable, Options cancels)
```

Each row: name, weights size, licence, state, backend time; the row the converter would load is marked. Delete asks "Delete SHARP? 3.9 GB". Input map from §4.4, no new buttons: Cross download/accept, Circle back, Options delete or cancel, L1/R1 page the licence text.

**Where downloads come from** — only a fully pinned spec may be offered:

| Entry | Official source | Size | Pinned SHA-256 | Licence | Gate |
| --- | --- | --- | --- | --- | --- |
| SHARP | `ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt` | 2,809,738,232 B | `94211a75…073d` | apple-amlr, research only | yes |
| DA2-Small | `huggingface.co/depth-anything/Depth-Anything-V2-Small-hf/resolve/main/model.safetensors` | 99,173,660 B | `3152477c…6b70` | Apache-2.0 | no |
| MoGe-2 S/B/L | `github.com/microsoft/MoGe` model card | 0.14/0.42/1.31 GB | to pin | MIT (+Apache DINOv2) | no |
| DA3-Small / Metric-L | `github.com/ByteDance-Seed/Depth-Anything-3` model card | 0.14 / 1.34 GB | to pin | Apache-2.0 | no |

The last two stay disabled until URL, size and hash are pinned and a GGUF profile exists; the licences are those in `docs/research/single-image-3d-models.md`, where `DA3-LARGE-1.1` is left out (Apache tag, CC-BY-NC model zoo).

**Where the weights live** (manager state dir `/data/ps5-splat-vr/state`):

```
state/licenses.json             acceptance hashes
state/models/<backend>.json     record; weights in <backend>.gguf (1.2 GB / 47 MB)
state/downloads/<file>[.part]   source checkpoint: SHARP 2.6 GB, DA2-S 95 MB
state/inbox/                    offline drop (USB/FTP), exact official name
```

Peak for SHARP is 2.6 + 1.2 = 3.9 GB until Delete; free space on `/data` was never measured (M3). The manager writes `<backend>.gguf` (`model_manager.cpp:230-232`) while the benches read fixed names under `/data/ps5-splat-vr/models/` (`platform/ps5/bench/sharp_bench.cpp:5,99`, `da2_bench.cpp:5,100`, `platform/ps5/scripts/push-data.sh:126-129,173-198`) — decision 8.

**How the converter picks the model.** One function, shown on screen, not a second truth: `resolve_backend()` prefers `--model`, then a ready manager record, then a `<data>/models/<backend>.gguf` that verifies, else the fallback, and logs why (`platform/desktop/app_mode.cpp:193-236,553-554`, `docs/desktop-app.md:79-91`); the panel prints that record as "converter uses: …". `fetch()` picks the converter by `spec.name` with an if/else (`model_manager.cpp:493-504`) and needs a dispatch table for MoGe/DA3. On the PS5 neither the manager nor the converter is built (`platform/ps5/ggml/CMakeLists.txt:70-90`), so nothing converts there yet (M2).

**Release packages.**

| Package | Contents | Size |
| --- | --- | --- |
| A. core (dev) | `PPSA99202/`: `eboot.bin` 24.5 MiB, `libc.prx` 1.2 MiB, `param.json` + `icon0.png`, `NOTICES`, `SHA256SUMS` | ~27 MiB, no weights |
| B. core + DA2-S | A plus `models/dav2s-f16.gguf` (47 MB, converted from the pinned safetensors) and the Apache-2.0 notice | ~74 MB, works offline |

Sizes are today's title build (`platform/ps5/title/README.md:25-31`) plus weights. In A, DA2-Small is offered ungated and SHARP waits for the licence. B bundles a *converted* GGUF, which spec rule 1 forbids in as many words (`docs/spec.md:65`) — decision 10. A packaging test asserts no `.pt`/`.safetensors` and nothing over 2 MiB but the eboot and the optional GGUF (M5).

## 5. New dependencies (each needs user approval)

1. **font8x8** (public domain, header-only) — 8x8 glyphs for option A.
2. **jxrlib** (BSD-2) — only for HDR captures; adds a decoder and a tone-map to SDR, and its sample decoder rejects the PS5's 16-bit PQ files, so it is M/L (§1.4). Otherwise the user sets screenshots to PNG, or exports via USB as JPG.
3. **Dear ImGui** (MIT) — only if option B is chosen; 5 core + 2 backend files.
4. Nothing new for Sense input (SDK stubs + existing export table) or HTTP (cpp-httplib is vendored), but `core/jobs`, `core/ingest` and `core/model` must be added to the PS5 cross build, plus a TLS-capable `IHttpFetcher` there (M1).

## 6. Task breakdown

| # | Task | Size |
| --- | --- | --- |
| T1 | `core/gallery`: albums, scene listing, meta fields, tests | M (2-4 d) |
| T2 | photo sources + read-only scans (inbox only in v1; USB/captures per §1.4) | M (2-3 d) |
| T3 | UI renderer: quad pipeline, atlas, font, TV + VR placement | L (5-8 d) |
| T4 | UI model + screens + navigation (menu/photos/albums/queue) | L (5-7 d) |
| T5 | input: DualSense d-pad nav, Sense open/register/read, laser ray | M (3-4 d) |
| T6 | PS5 session glue: ingest + queue + guard + album-aware runner | L (4-6 d) |
| T7 | spike: can a title read `/user/av_contents`? (gates T2) | ~~S (0.5 d)~~ **done 2026-10-26: no — errno 2 on every `/user` and `/mnt/usb*` root, `/mnt/ext1` only (§1.3)** |
| T8 | thumbnails: photo decode, scene render-once cache | S/M (1-2 d) |
| T9 | console verification (photos, albums, conversion, VR/TV) | S/M (1-2 d) |
| M1 | PS5 HTTPS fetcher for `IHttpFetcher` (HEAD, resume, licence text) | M (2-3 d) |
| M2 | `core/model` + converter in the PS5 build; on-console SHARP conversion time and memory | M (2-4 d) |
| M3 | free-space guard, per-backend converter dispatch, pinned MoGe-2/DA3 specs | S/M (2-3 d) |
| M4 | model screen: list, licence pager, progress, cancel, delete, "converter uses" | M (2-3 d) |
| M5 | release packaging: no-weights zip, DA2-S variant, notices, no-weights test | S (1 d) |

S <=1 day, M 2-4, L 5-8; browsing roughly 25-30 dev-days (T7 first), models 9-14 more (M5 and M2 last).

## 7. Open decisions

1. **Captures**: ~~run the T7 spike to read `/user/av_contents`~~ *answered (§1.3) and routed (§1.4)* — no title-side access. Recommended: v1 = inbox only (D), v2 = companion payload (C), HEN app jailbreak (A) only if the user moves to OnionHEN — allowlist configurable there, its promotion depth (§1.4 A) still unverified. Confirm inbox-only v1 and the helper payload in v2?
2. **HDR captures**: *§1.4: jxrlib rejects PS5's 16-bit PQ JXR as-is (M/L)* — ask for PNG on SDR captures and the USB JPG export for HDR, and defer jxrlib?
3. **Album = folder** under `scenes/` (recommended, zero migration), or a JSON playlist index that can hold one scene in several albums?
4. **Album naming**: auto date names + rename on the LAN page (recommended), or build an on-screen keyboard?
5. **Panel**: minimal own panel (recommended) or Dear ImGui?
6. **Sense pointer**: right controller only, trigger = select (recommended)?
7. **Transport**: console HTTPS fetcher (M1), or a phone/PC relay through the LAN page (needs streaming upload, `ingest_server.cpp:186-192`)? Either way the licence text is paged on the panel and its SHA-256 is pinned, so a relayed copy can be checked.
8. **GGUF path**: `state/models/<backend>.gguf` as the single truth (recommended) and drop `/data/ps5-splat-vr/models/`, or keep both names with a symlink?
9. **Delete**: remove source + GGUF (frees 3.9 GB for SHARP, keeps the acceptance) or keep the source for a cheap re-conversion?
10. **Bundled DA2-S variant** vs spec rule 1 ("no converted GGUF"): amend the rule to allow Apache-2.0 conversions, or bundle the 95 MB safetensors and convert on first run?
11. **Unoffered models**: show MoGe-2/DA3 as disabled rows with "not offered yet", or hide them until a pin and a GGUF profile exist (recommended)?
