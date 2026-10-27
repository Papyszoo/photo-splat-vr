# UI v1 design notes

Design notes for the in-app UI: the panel shown in the PSVR2 headset and on the
TV. Product decisions are in [`PLAN.md`](PLAN.md); the source survey is
[`research/browse-ui.md`](research/browse-ui.md) (its recommendations apply
wherever `PLAN.md` is silent).

## 1. Goals

In-app, on the console and the desktop build: download models, browse photos
(inbox, PS5 screenshots, USB), convert with the chosen model, browse scenes by
album (album = folder under `scenes/`), organise them, view them. The same panel
UI shows in the PSVR2 headset (world-locked quad) and on the TV (1:1 full
frame). Also queued: Sense grab (move/rotate/two-hand scale in place),
DualSense/Sense controller pictures with tooltips and a hide-tooltips button that
stays highlighted while off.

Non-goals: jxrlib/HDR captures, DA3/MoGe offers (hidden until pinned), ImGui,
LAN-page album editing, cloud anything.

## 2. Decisions

- **Panel**: our own, CPU-rasterised. `UiCanvas` (1280x720 RGBA, font8x8 text,
  rects, scaled image blits) is drawn on the CPU and uploaded as ONE overlay
  texture through
  `SplatRenderer::add_overlay_rgba_texture/update_overlay_rgba_texture/set_overlay_quads`
  (OverlayRenderer's RGBA8 texture, `overlay_rgba.frag`), so a photo thumbnail
  keeps every colour; no new pipeline beyond that fragment shader, no atlas. The
  R8 mask + tint path stays for the scene label and HUD. The same canvas is
  PNG-dumpable on the desktop (`tools/ui_preview`) with no console.
- **Input map** (identical on DualSense and Sense, `pad_input.h` stays the one
  table). In a scene: Options = open menu (no longer quits), Cross hold 1 s =
  recentre, L1/R1 = previous/next scene in the album, L2/R2 shrink/grow, sticks
  move/turn, Triangle = toggle tooltips, Create untouched. In the UI:
  d-pad/left stick = move focus, Cross = select, Circle = back, L1/R1 = page/tab,
  Square = secondary action (select toggle / delete / cancel), Triangle =
  tooltips, Options = close menu (back to the scene if one is loaded). Quit =
  menu item "Quit" (`LoadExec("exit")`). Sense extras: right-controller laser
  ray hovers, R2 trigger = click.
- **TLS = native sceHttp/sceSsl/sceNet**, not mbedTLS. Why: no new dependency;
  the console's own CA store; the SDK already ships
  `libSceHttp/libSceSsl/libSceNet` stubs (`ref/radv-ps5/sdk/target/lib`);
  ProsperoMgr (GPL-3.0, same toolchain) dropped libcurl+mbedTLS for exactly this
  API (`ref/ProsperoMgr/Makefile:68-78`,
  `source/backend/src/ps5_http_bridge.c`) and downloads with it on this console.
  Unproven from a title (not a payload): a probe (`BENCH=http`) tests it on the
  console. If it fails: fallback is the model manager's offline inbox import
  (already built) and, only with user approval, mbedTLS (Apache-2.0, as PS5LM
  does).
- **Captures** via a companion payload `splat-helper` with a file mailbox, no
  sockets: the title writes `/data/ps5-splat-vr/helper/req.json`, the helper
  (payload, sees `/user/av_contents`) answers in `resp.json` and copies chosen
  JPG/PNG captures into `inbox/`. The title starts it by pushing the ELF to
  elfldr `127.0.0.1:9021`. HDR `.jxr` captures are listed as "HDR, not
  supported".
- **USB**: list `/mnt` for `usbN` aliases at runtime (ShadowMountPlus);
  unconfirmed with a stick, so the source row appears only when `/mnt/usbN`
  opens.
- **GGUF path**: one truth, `state/models/<backend>.gguf` in the manager's state
  dir; a verified legacy `/data/ps5-splat-vr/models/<backend>.gguf` is accepted
  as ready (no copy). Delete removes source and GGUF, keeps the licence
  acceptance.
- **Models**: SHARP (recommended; best quality, research licence, accept on
  screen) and DA2-Small (fast, approximate scene size, ungated); only pinned
  entries. The selection is stored in `state/ui.json`.
- **Names**: new folders get a date name (`2026-10-26`, `-2` on collision);
  rename/new via the panel keyboard; the PS5 IME dialog (`libSceImeDialog` stub
  exists) is tried in a console test only, panel keyboard is the shipped path
  unless the IME works in VR.

## 3. Architecture and contracts

Layers (each testable without the one above): `core/gallery` (fs, no UI) ->
`core/ui` (canvas, input, screens, services interfaces; no Vulkan, no platform)
-> `platform/ps5/viewer` `UiHost` (canvas -> overlay quad, TV and VR) ->
services adapters (`core/ui/services_*`: jobs, models; PS5 specifics under
`platform/ps5/`).

**core/gallery**: namespace `splat`:
`AlbumEntry{Path dir; std::string name; size_t scene_count; Path cover;}`,
`SceneEntry{Path folder; std::string name, album; SceneMeta meta; bool has_thumb;}`,
`PhotoSource{id,label,Path dir,bool removable}`,
`PhotoEntry{Path path; std::string name; uint64_t size; int64_t mtime;}`;
`list_albums(root)` (album "" = loose scenes, shown as "All scenes"),
`list_scenes(root, album)`, `create_album`, `rename_album`, `delete_album` (empty
only), `move_scene(root, folder, to_album)`, `default_album_name(time_t, root)`,
`discover_sources(const SourceRoots&)`, `list_photos(source)`
(jpg/jpeg/png, case-insensitive, depth <= 3, newest first),
`load_thumb(path, max_edge)` -> `Image` RGBA. `SceneMeta` gains optional
`captured_at`, `source` (old files still read). `ScenePlaylist::from_entries` for
an album.

**core/ui**: `UiCanvas` (`clear, fill_rect, stroke_rect, text(x,y,str,scale,color)->width, text_width, blit(x,y,w,h,Image), bar(x,y,w,h,frac), pixels(), size, dirty, write_png`),
`UiTheme`. `UiInput{uint32_t pressed, held; bool pointer_valid; float px, py; bool click; double dt;}`
with `enum UiAction: Up Down Left Right Select Back PagePrev PageNext Secondary Tooltips Menu`.
`IPanelContent{update(const UiInput&), draw(UiCanvas&) -> bool changed}`.
`Screen{on_enter, update, draw, hints}`, `ScreenStack`, `UiApp : IPanelContent`
(owns the stack, `take_command() -> optional<UiCommand>`;
`UiCommand{ViewScene{album,index}, ResumeScene, Quit}`). Widgets: `ListView`,
`GridView` (focus, paging, pointer hit test, scroll into view). Services in
`core/ui/services.h`:
`IJobService{enqueue(photos, backend, album), snapshot()->JobView[], cancel(id), busy()}`,
`IModelService{list()->ModelView[], licence_text(id), accept(id), download(id), cancel(id), remove(id), select(id), selected()}`,
`ICaptureService{available(), refresh(), list(), copy_to_inbox(paths), state()}`.
Each has a deterministic fake in `core/ui/fakes.h` (used by tests, `ui_preview`
and fake-mode console runs).

**platform/ps5/viewer**: `UiHost{UiHost(SplatRenderer&, IPanelContent&, Mode tv|vr); init(); frame(UiInput, head pose); visible(); set_visible(); quads(); }`
— the only thing `main.cpp`/`vr_mode.cpp` talk to. viewer.env keys added: `UI=1`
(start in the menu), `UI_FAKE=1` (fake services), `SCRIPT=<path>`, `SHOTS_DIR`
(default `/data/ps5-splat-vr/shots`).

**Script + frame dump**: `core/ui/input_script`: text lines
`<seconds> press|release|hold <Button> [secs]`, `<s> stick <lx> <ly> <rx> <ry>`,
`<s> shot <name>`, `<s> quit`, `#` comments; times are seconds from the first UI
frame; the viewer feeds the events into the same action path as real pads and
writes `shot` PNGs (TV readback -> `encode_png` in `core/jobs/image_io`) to
`SHOTS_DIR/<name>.png`. Log lines: `viewer: script shot <name>`, `viewer: script
done`. `platform/ps5/scripts/ui-test.sh` installs, writes `viewer.env`+script,
launches, waits for `script done`, pulls PNGs read-only (`GET
/api/ftp/download`) to `$SPLAT_DATA_DIR/runs/ui_v1/shots/<run>/`, restores
`viewer.env`.

**Conversion**: `ConvertSession{open(model)/run(photo,out,progress)/close()}` over
`SharpReconstructor`; `JobService` wraps `JobQueue`+`JobRunner` (album-aware,
scene folder = photo stem). Link problem (duplicate `vkGetDeviceProcAddr`,
`vkCmdCopyBuffer`, `vkGetPhysicalDeviceFeatures2` between `vk_entrypoints.cpp`
and `ps5_vulkan.cpp`; glue-only `fork/execlp/pathconf`;
`--wrap=ggml_backend_graph_compute`) is solved build-only. Two runtime designs
were measured on the console: **S1** in-process (renderer alive but scene
unloaded, ggml-vulkan on its own device, progress bar keeps animating; peak
7.2 GiB RSS + small renderer fits 11.44 GiB) and **S2** (the UI process never
initialises ggml; the title relaunches itself in convert mode and back, through
the resident ProsperoMgr's loopback `/api/titles/launch` or `LoadExec`; the one
eboot still contains both modes). S1 is the shipped path.

## 4. Components

The numbers are kept because code comments refer to them.

| # | Component | Area |
|---|-----------|------|
| 1 | gallery core | core/gallery, core/scene meta, core/render/scene_playlist |
| 2 | ui canvas | core/ui canvas/theme/input headers, image_io `encode_png`, tools/ui_preview |
| 3 | http fetcher (sceHttp) + BENCH=http probe | platform/ps5/net, platform/ps5/bench |
| 4 | sharp link in viewer (build only) | platform/ps5/viewer/Makefile, platform/ps5/ggml |
| 5 | capture helper payload + client | platform/ps5/helper |
| 6 | input script parser | core/ui/input_script |
| 7 | ui framework + menu | core/ui |
| 8 | tv panel + frame dump + script wiring | platform/ps5/viewer, scripts |
| 9 | console smoke: panel pattern, shots, http probe | runs only |
| 10 | screens: albums/scenes | core/ui/screens |
| 11 | screens: photos/convert dialog | core/ui/screens |
| 12 | jobs service + queue screen | core/jobs, core/ui |
| 13 | models service + screens | core/model adapter, core/ui |
| 14 | keyboard + album management | core/ui/screens |
| 15 | viewer integration TV (action map, UiApp in TV, real gallery) | viewer, pad_input |
| 16 | viewer integration VR (panel quad, Options=menu, album playlist) | vr_mode |
| 17 | console browse test (scripted TV tour) | tests only |
| 18 | console+code convert spike (S1/S2) | viewer |
| 19 | PS5 convert service + queue wiring | platform/ps5 |
| 20 | console convert test | tests only |
| 21 | PS5 models/net wiring | platform/ps5, core/model build |
| 22 | console models test | tests only |
| 23 | captures/USB wiring | platform/ps5, core/ui |
| 24 | console captures test | tests only |
| 25 | Sense pointer | pad_input, vr_mode |
| 26 | Sense grab (logic + wiring) | core/render/grab, vr_mode |
| 27 | controller pictures + tooltips | core/ui/screens, vr_mode |
| 28 | final console tour | tests only |

## 5. Dependency order

Gallery and canvas first, then the UI framework and the TV panel, then the
screens; viewer integration (TV, then VR) after the screens; PS5 services and
their console tests after the viewer wiring. `platform/ps5/viewer/Makefile`,
`main.cpp` and `vr_mode.cpp` are touched by several components, so those changes
land one after another. Console tests never overlap: the console is
single-user.

## 6. Scripted input and console test method

All console runs are `MODE=tv` with `UI=1` and a `SCRIPT`: the script presses
buttons at timestamps, `shot` lines write PNGs of the TV frame, `quit` ends the
title with `PASS`. `ui-test.sh` wraps install/launch/klog/pull. Each script is
kept in `platform/ps5/scripts/ui-tests/*.txt` together with the expected klog
lines; the shots go to `runs/ui_v1/shots/<run>/`. VR-only code (panel quad
placement, pointer ray, grab) is verified by host unit tests with scripted
poses and by hand in the headset.

Safe-by-default console rules for every console run: only the title's `/data/homebrew/<TITLE_ID>` (dev `PPSA99202`) and
its data folder `/data/ps5-splat-vr` are touched (agents never run or touch the release install `PPSA99770` / `/data/photo-splat-vr`); `viewer.env` is saved first and restored by
sha256; nothing under `scenes/` is deleted (tests use `scenes_test/` copies or
albums they create and clean by name); fake services unless the run is about the
real one; a watchdog (`SECONDS`) ends every run.

## 7. Gates (every change)

Desktop: configure `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`,
build all, run `ctest --test-dir build`. PS5-touching changes additionally:
`platform/ps5/ggml/build.sh`, `make -C platform/ps5/viewer` (or the named
Makefile) and `verify-package.sh`. No change claims a console result it did not
measure.

## 8. Risks

1. **SHARP in the viewer eboot**: duplicate-symbol link, two Vulkan devices in
   one process on RADV, 7.2 GiB peak. Fallback S2 (relaunch) is built in the
   same component. Last resort: conversion stays the bench title's
   `BENCH=convert` driven by a script and the UI only browses/queues (then the
   queue screen shows "run converter" and the user relaunches).
2. **sceHttp from a title** unproven; offline inbox import keeps models usable.
3. **Title cannot write some paths**: `/data/ps5-splat-vr` writes are proven
   (scenes, logs, bench.env); `state/` and `shots/` are the same mount.
4. **Large VR source file**: all VR edits are small hooks behind `UiHost`.
5. **Frame dump format**: TV readback layout (BGRA vs RGBA) must be checked once
   or the shots have swapped colours.
6. **Thumbnails on the console**: console scenes may have no `thumb.jpg`; the
   gallery renders a labelled placeholder and new scenes write `thumb.jpg`; an
   on-demand render-once cache is out of scope.

## 9. Open questions at the time of the build

- HDR `.jxr` screenshots: v1 lists them as unsupported (the only capture on the
  console is HDR). jxrlib (BSD-2) is a possible later add; or set Captures to
  SDR PNG.
- mbedTLS only as a fallback if sceHttp fails from a title.
- New files on the console: `/data/ps5-splat-vr/{shots,helper,state}` and
  `splat-helper.elf` pushed to elfldr (loopback), nothing outside
  `/data/ps5-splat-vr`.
- The Options button no longer quits the viewer (quit moves into the menu).

## User decisions 2026-10-27 (override the layout above where they differ)

- The UI is for browsing existing scenes first: folders (albums) of scenes, then
  the scenes; opening a scene shows it.
- No top-level Photos section. Converting new photos: a "+" inside a folder opens
  a file picker (inbox, PS5 captures, USB), then a model choice; the result lands
  in that folder.
- Models (download, licence, delete) live in Settings.
- Not cluttered: few items on screen, actions such as rename/move/delete behind
  a context button, not as visible buttons.
- "Native PS5" means the visual style (dark, clean, smooth font, rounded tiles,
  clear focus), not the PS5 home layout.
- A smooth open-licence font (OFL, drawn with stb_truetype) replaces the 8x8
  pixel font in the panel UI.
- Controller help: 3D in VR. Simple own-made Sense controller models at the
  tracked hands with floating labels, a 3D DualSense floating in front when it
  is used; hide-tooltips button highlighted on the model while labels are off.
  No Sony assets.
- Photo picker sources: Inbox (`/data/ps5-splat-vr/inbox`, the FTP upload
  target, path shown), Console (folder browser over title-visible roots `/data`
  and `/mnt/ext*`), USB, Captures.
