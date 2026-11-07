# Photo Splat VR

Turn one photo into a 3D Gaussian splat scene on a jailbroken PS5 and stand
inside it with PlayStation VR2. The reconstruction model, the Vulkan splat
renderer and the panel UI share one core; only a thin platform layer differs
between the console and a desktop build.

The SHARP backend is an independent reimplementation of the
[SHARP](https://github.com/apple-aiml-research/ml-sharp) architecture on ggml,
written from the paper and the reference code's observable behaviour. No Apple
code and no model weights are part of this repository.

v0.6 builds on the library features of v0.5 (photos in the common camera and
web formats, splat import, rotate left or right on any photo or scene entry, and
network libraries over SMB and NFS): the photo picker browses folders on every
source, scenes can be saved straight to a network folder, the TV shows its own
picture and menu, and the SHARP model downloads about three times faster.
v0.1 does what the project set out to do: photos are converted on the console
and the resulting scenes display in VR and on the TV. DA2-Small ships as an
alternative, with approximate scene size.

- Repository: <https://github.com/Papyszoo/photo-splat-vr>
- Specification: [`docs/spec.md`](docs/spec.md)
- Decisions: [`docs/PLAN.md`](docs/PLAN.md)
- Known issues: [`docs/BACKLOG.md`](docs/BACKLOG.md)

## What is new in 0.6

- **Folders in the photo picker.** Every source (Console, Inbox, USB, network
  locations) is a folder browser like Finder or Windows Explorer: folders first,
  then the photos and splat files of the folder you are in. **Cross** opens a
  folder, **Circle** goes up. **Square** on a folder no longer imports every
  splat file below it: you pick the files yourself, and **Cross** on a splat
  file imports it, from any source.
- **Output folder.** `Settings > Output folder` chooses where new scenes are
  saved: the console (default) or a folder on a network location, picked by
  browsing that location's folders (**Square** = "Use this folder"). Scenes go
  to `<output folder>/<album>/<scene>`, so albums look the same as on the
  console. Existing scenes stay where they are.
- **Albums on network scene libraries.** A network scene library lists the
  folders that hold scenes as albums, and the scenes directly in the folder as
  Unsorted. An offline share shows its own error.
- **Offline share, no lost scene.** A scene whose network folder does not
  answer shows "Waiting for <location>", stays on the console, tries again by
  itself and is saved when the share is back. **Square** on the row cancels it.
  A scene is saved under a hidden temporary name and only gets its real name once
  it is whole, so a share that drops never leaves a half-saved scene.
- **Faster model download.** The SHARP checkpoint (2.8 GB) downloads over four
  connections at once, about 3 times faster on the console, and the Models row
  shows "about N min left". A paused or interrupted download continues where it
  stopped, and the checkpoint is still verified after it arrives.
- **The TV has its own picture.** In VR the TV shows the scene without the VR
  window or HUD and, while the menu is open, the same menu flat on the whole
  screen. Anyone holding a controller can drive it, and it works before the
  headset is on. If the console refuses the TV output, the TV mirrors the
  headset's view as before.
- **Scene and menu start in front of you.** The headset's tracker starts about
  90 degrees off and corrects itself a few seconds later; the app now notices
  and puts the scene and the open menu in front of where you face.
- **Hold Options to recentre.** Hold **Options** for 1 second to recentre the
  scene and the open menu, like the PSVR2 standard. A tap on **Options** still
  opens and closes the menu, now when you let go.
- **Steadier menu window.** Moving or turning your head no longer skews the
  contents of the VR menu window.

## Formats

The photo extensions and decoders are listed in
[`docs/formats/photo-formats.md`](docs/formats/photo-formats.md); the splat
readers, their guards and the thinning rule are in
[`docs/formats/splat-formats.md`](docs/formats/splat-formats.md). Those two
tables are the overview; this page does not repeat them.

## Network libraries

`Settings > Network locations` adds an SMB share first (Windows, macOS and most
NAS boxes speak it) or an NFS export. The console's title has no name resolver,
so a location names its server by **IP address**. A location can be a photo
source (a tab in the picker), a splat library (scenes in the folder grid), or
both. Scenes on a share open where they lie, without a copy on the console.

> **Credentials are stored in plain text.** The password is written to
> `state/locations.json` on the console (`/data/photo-splat-vr/state/` for the
> release install). Anyone with FTP or file access can read it, it is not
> encrypted at rest, and it travels over the network in clear text unless SMB3
> encryption is on. Use a dedicated account that can only read the folder, and
> enable **Encrypt** when the server supports it.

## Requirements

- A jailbroken PS5 on a supported firmware (7.00 to 13.60; 13.60 is the tested
  one) with a foreground user signed in.
- A homebrew loader that registers titles from `/data/homebrew/<title id>`; this
  release installs as **PPSA99770** ("Photo Splat VR") with ShadowMountPlus. The
  development build is **PPSA99202** ("Photo Splat VR Dev"); both can be
  installed at once and each keeps its own data folder (see
  `platform/ps5/title/README.md`).
- PS VR2 for VR viewing. A DualSense or the two PS VR2 Sense controllers for
  input. TV mode works without the headset. In VR the TV shows the scene (without
  the VR window or HUD) and, while the menu is open, the same menu flat on the
  screen; whoever holds a controller can drive it, and it works before the
  headset is on.
- Network access on the console for the model download, and FTP access to the
  console for copying the package and your photos.
- About 4 GB free under `/data` for the SHARP checkpoint and its converted form;
  DA2-Small needs about 150 MB.

## Install

Download the release zip and unzip it. It contains the title folder
`PPSA99770/`:

```
PPSA99770/
  eboot.bin
  sce_module/libc.prx
  sce_sys/param.json
  sce_sys/icon0.png
  shaders/*.spv
  helper/splat-helper.elf
```

The release carries no model weights.

1. Connect to the console with an FTP client that can write under `/data`
   (ShadowMountPlus's FTP service, or any other FTP server your loader
   provides).
2. Create `/data/homebrew/PPSA99770/` and upload the **contents** of the
   unzipped `PPSA99770/` folder there, keeping the `sce_module/`, `sce_sys/`,
   `shaders/` and `helper/` subfolders. The result must be
   `/data/homebrew/PPSA99770/eboot.bin` and so on.
3. Ask ShadowMountPlus to rescan `/data/homebrew` (its web UI or API), or reboot
   the console and let the scan run. The tile **Photo Splat VR** then appears on
   the home screen.
4. Start it from the home screen. The app keeps its inbox, scenes, models and
   state under **`/data/photo-splat-vr/`** (the release install's own data
   folder; the dev build `PPSA99202` uses `/data/ps5-splat-vr/`). It creates the
   folders it needs as you use them, including its downloaded models, so the two
   installs share nothing.

If the tile does not appear, check that `sce_sys/param.json` arrived with the
rest and that the loader sees the folder. The same install is done by
`platform/ps5/scripts/title.sh install build/ps5/title/PPSA99770` when building
from source with `RELEASE=1`.

## First run

### Models

Open **Settings → Models**. Nothing is installed on a fresh console.

- **SHARP** is the recommended model: best quality and slower. It is gated by
  Apple's model licence. Select it, read the licence on screen, and accept it.
  The console then downloads the checkpoint (about 2.8 GB), verifies its
  SHA-256, and converts it to the runtime file used for conversion (about
  1.2 GB) on the console. Nothing is uploaded anywhere.
- **DA2-Small** is the fast model, with approximate scene size. It is
  Apache-2.0 and needs no acceptance; its checkpoint is about 100 MB.

How long it takes: the SHARP download is about 2.8 GB, a few minutes on a
wired connection (the console fetches it over four connections at once), plus
about 4 minutes of preparing it afterwards. The row shows "about N min left"
while the download runs; verifying and preparing show their stage and percent
only.

A download or conversion can be paused and resumed. **Delete** removes the
downloaded files but keeps the licence acceptance. The **Default model** row
picks the model offered first when you convert; SHARP is used when it is ready.

### Photos

Photos can come from five sources, all inside the app:

- **Inbox** — upload photos from a phone or PC to
  `/data/ps5-splat-vr/inbox/` over FTP. JPEG, PNG, WebP, TIFF, HEIC/HEIF, AVIF,
  BMP, GIF, TGA and camera RAW are accepted; the app watches this folder.
- **Console** — browse the folders the title can see under `/data` and on
  mounted USB volumes, and copy photos into the Inbox. Splat files
  (`.ply`, `.splat`, `.spz`, `.compressed.ply`) are listed here and in the Inbox
  and USB tabs, and are imported with **Cross**.
- **USB** — one tab per stick mounted under `/mnt` (ShadowMountPlus aliases each
  mounted volume for the title).
- **Network** — one tab per SMB or NFS location marked as a photo source.
  See [Network libraries](#network-libraries).
- **Captures** — PS5 screenshots. The app starts its bundled helper payload
  itself. This source needs a payload loader (elfldr) on the console's loopback;
  without one, the other four sources still work. HDR `.jxr` captures are
  listed as unsupported; set screenshots to SDR.

### Convert and view

Open or create a folder in **Scenes**, press **+** in the folder, pick photos,
then choose a model. Each photo becomes a scene tile with its own progress. A
finished scene opens in the headset (or on the TV) and stays world-locked; move
with the sticks, or grab it with a Sense controller. Press **L3/R3** on a photo
before converting, or on a finished scene, to turn it 90 degrees about the
viewing axis; the turned version replaces the old one.

## Controls

The mapping is the same on the DualSense and the two Sense controllers. A Sense
controller splits the buttons: left = L1, Create, Square/Triangle, L3; right =
R1, Options, Cross/Circle, R3.

| Action | DualSense | Sense controllers |
| --- | --- | --- |
| Menu / back to scene (tap, on release) | **Options** | right: **Options** |
| Recentre the scene and the open menu window (hold 1 s) | **Options** | right: **Options** |
| Recentre (hold 1 s; the window while the menu is open) | **Cross** | right: **Cross** |
| Previous / next scene | **L1** / **R1** | left **L1** tap / right **R1** tap (a hold grabs) |
| Walk (forward/back, strafe) | **left stick** | left controller's **stick** |
| Rise/sink, turn right/left | **right stick** | right controller's **stick** |
| Shrink / grow the scene | **L2** / **R2** | left **L2** / right **R2** |
| Grab (move/turn the scene) | — | **L1 hold** (left) / **R1 hold** (right); both grips scale between the hands |
| Rotate a photo or scene entry | **L3** / **R3** | left **L3** / right **R3** |
| Toggle control tooltips | **Triangle** | **Triangle** |
| Quit | **Settings → Quit** | **Settings → Quit** |

In the panel UI: d-pad or left stick moves the focus, **Cross** selects,
**Circle** goes back, **Square** is the secondary action (select, delete,
cancel), **L1/R1** page through tabs, and **L3/R3** turn the focused photo or
scene entry. With the Sense controllers, point with the right controller and
press **R2** to click.

Settings holds comfort options (move speed, turn speed, scale pivot, edge
softness, blending), the **Thumbnail size** slider, and the Models, Conversions,
Network locations and Visible folders screens.

### Visible folders

If someone visits, you can hide **scene folders** you do not want them to
browse. Open **Settings → Visible folders**, set a passcode (4 to 32 characters,
typed with the controller), and tick the albums, **Unsorted**, **Other scenes**
and network locations that are scene libraries. Everything below a tick is
visible; everything else is gone from the shelf — albums, scenes, thumbnails,
the queue and the last opened scene — in VR and on the TV. A folder that appears
later stays hidden until you tick it.

This list covers scenes only. The **photo picker is not filtered**: with the
passcode on, a visitor can still open + → Photos and browse the Inbox, the
console's folders, USB sticks, captures and every photo on them. Hiding photos
is not what this passcode does.

This hides scene folders in this app and does not encrypt them: anyone with the
share, FTP or another app can still open the files.

Put each person or family in its own top-level folder or its own network
location; a tick publishes a whole subtree. The passcode is stored as a salted
hash, never as text. If you forget it, delete
`/data/ps5-splat-vr/state/visible-folders.json` from a PC or FTP client and
relaunch: every scene folder is visible again. The long form, including the exact
rules and the limitations, is in [docs/visible-folders.md](docs/visible-folders.md).

### Network locations

**Settings → Network locations** adds, edits, tests and removes shares, with or
without a passcode. The photo picker lists every location that is a photo
source as a tab, and its last tab, **+ Add network location**, opens the same
editor with "Photos tab" already on, so a share can be added from where the
photos are looked for and its tab is there when the editor returns. See
[docs/network-locations.md](docs/network-locations.md).

## Build from source

Desktop development build, macOS or Linux:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/platform/desktop/splat_desktop
```

macOS also needs `molten-vk`, `vulkan-loader`, `vulkan-headers`, `vulkan-tools`
and `shaderc`; Debian/Ubuntu needs `libvulkan-dev`, `vulkan-tools`, `glslc`,
`spirv-headers`, `mesa-vulkan-drivers`, `xorg-dev`, `libwayland-dev`,
`wayland-protocols` and `libxkbcommon-dev`. The desktop build is for developing
the core and renderer; the console uses RADV.

PS5 title build (needs the external payload SDK, RADV archive and LLVM, see
[`platform/ps5/README.md`](platform/ps5/README.md)):

```sh
export SPLAT_DATA_DIR=/path/to/data       # default: the repo's parent directory
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG="$(brew --prefix llvm@21)/bin/llvm-config"

platform/ps5/ggml/build.sh
make -C platform/ps5/viewer              # dev build     -> build/ps5/title/PPSA99202
make -C platform/ps5/viewer RELEASE=1    # release build -> build/ps5/title/PPSA99770
platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99770
```

Console addresses and ports come from the environment or a gitignored
`.env.local` (copy [`.env.example`](.env.example)); nothing is hard-coded.
`platform/ps5/README.md`, [`platform/ps5/title/README.md`](platform/ps5/title/README.md)
and [`docs/console-runbook.md`](docs/console-runbook.md) cover the package,
install and console workflow in detail.

Repository layout: `core/` (scene format, renderer, reconstruction, jobs,
gallery, panel UI), `platform/desktop/` (GLFW viewer and app mode),
`platform/ps5/` (payloads, title package, viewer, scripts), `tests/` (doctest
suite), `tools/`, and `third_party/` (ggml and GLFW submodules plus vendored
stb, miniz, doctest, Inter, font8x8, nlohmann/json and cpp-httplib).

## Licences

Photo Splat VR is free software under the **GNU General Public License v3.0 or
later** (`GPL-3.0-or-later`). The full text is in [`LICENSE`](LICENSE).
Copyright (C) 2026 the Photo Splat VR contributors.

The PS5VR, HLA-PSVR2 and PS5_Vulkan code that was ported into this project is
itself `GPL-3.0-or-later`. Vendored third-party components keep their own
licences — MIT, OFL-1.1 and public domain — and are listed with their notices in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Model weights are **never shipped** and never hosted by this project. The app
downloads them from the model authors' official URLs, and you use them under
their own licences:

- SHARP: Apple's `LICENSE_MODEL`, research use only; the app shows the text and
  records your acceptance before downloading. The converted runtime file stays
  on your console.
- DA2-Small: Apache-2.0.

## Credits

- Apple ML research for the SHARP paper and reference implementation; the ggml
  backend here is an independent reimplementation.
- PS5VR (Husam Osman) and HLA-PSVR2 (theghostonline) for the PS VR2 runtime
  path.
- PS5_Vulkan and ps5-native-app-boilerplate (BlackBearReloaded, Mihawk-99) for
  the title packaging and VideoOut layout.
- PS5LM (cobanov) for the documented RADV and title-build behaviour.
- vkgs for the Gaussian projection and splat shaders.
- ggml, GLFW, stb, miniz, doctest, nlohmann/json, cpp-httplib, Inter and
  font8x8.
- Mesa/RADV, ps5-payload-sdk, ProsperoMgr, ShadowMountPlus and the PS5 homebrew
  community.
- Immich VR, used as a comparison reference for the renderer.

PlayStation, PS5 and PS VR2 are trademarks of Sony Interactive Entertainment.
This project is not affiliated with Sony.
