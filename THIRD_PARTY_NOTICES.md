# Third-party notices

This project is licensed under GPL-3.0-or-later (see `LICENSE` and `README.md`).
The components below are used under their own licenses. No model weights are
distributed with this repository.

## Bundled or linked in the desktop build

### ggml

- Version: `v0.27.0` (git submodule, `third_party/ggml`)
- Upstream: <https://github.com/ggml-org/ggml>
- License: MIT
- Used for: tensor library and the ggml-vulkan backend for the SHARP
  reimplementation.

### GLFW

- Version: `3.4` (git submodule, `third_party/glfw`)
- Upstream: <https://github.com/glfw/glfw>
- License: zlib/libpng
- Used for: desktop window, input and Vulkan surface creation.

### stb

- Files vendored: `stb_image.h`, `stb_image_write.h`, `stb_image_resize2.h`
  (v2.18), `stb_truetype.h` (v1.26), in `third_party/stb/`
- Upstream: <https://github.com/nothings/stb>
- License: public domain (dual-licensed MIT where noted in each file)
- Used for: image decode/encode and resizing; rasterising the panel font.

### Inter

- File vendored: `Inter-Medium.ttf` (Inter 4.1), unmodified, in
  `third_party/inter/` (source, version and SHA-256: `third_party/inter/README.md`)
- Authors: The Inter Project Authors
- Upstream: <https://github.com/rsms/inter>
- License: SIL Open Font License 1.1 (`third_party/inter/OFL.txt`)
- Used for: the panel UI's text, compiled into the binary.

### font8x8

- File vendored: `font8x8_basic.h`, unmodified, in `third_party/font8x8/`
  (source and retrieval note: `third_party/font8x8/README.md`)
- Author: Daniel Hepper, based on Marcel Sondaar's and IBM's public-domain
  8x8 VGA fonts
- Upstream: <https://github.com/dhepper/font8x8>
- License: public domain
- Used for: the scene label's text mask (`core/render/text_raster.cpp`),
  rasterised once per text change into an R8 overlay texture (the panel UI
  uses Inter instead).

### miniz

- Version: `3.1.2`, vendored in `third_party/miniz/`
- Upstream: <https://github.com/richgel999/miniz>
- License: MIT
- Used for: zip reading for the `.pt` checkpoint converter.

### doctest

- Version: `2.4.12` (`third_party/doctest/doctest.h`)
- Upstream: <https://github.com/doctest/doctest>
- License: MIT, Copyright (c) 2016-2023 Viktor Kirilov; full text in
  `third_party/doctest/LICENSE.txt`
- Used for: unit tests.

### nlohmann/json

- Version: `3.12.0`, single header at `third_party/json/nlohmann/json.hpp`
- Upstream: <https://github.com/nlohmann/json>
- License: MIT (`third_party/json/LICENSE.MIT`)
- Used for: reading and writing `meta.json` scene metadata.

### cpp-httplib

- Version: `0.18.3` (release tag `v0.18.3`), single header at
  `third_party/httplib/httplib.h` (SHA-256
  `a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5` — verify on
  upgrade)
- Upstream: <https://github.com/yhirose/cpp-httplib>
- License: MIT, Copyright (c) 2017 Yuji Hirose
- Used for: the C9 LAN upload page server (`core/ingest`), built with its
  default POSIX socket/thread path (no OpenSSL, no zlib).

### vkgs

- Upstream: <https://github.com/jaesung-cs/vkgs>
- License: MIT, Copyright (c) 2024 jaesung-cs
- Used for: the C6 renderer's Gaussian projection and EWA covariance
  conventions, and the instanced alpha-blended quad shaders, ported from
  `src/shader/projection.comp`, `src/shader/splat.vert` and
  `src/shader/splat.frag`. The port rewrites the buffers for the `.psplat`
  fp16 layout, adds per-eye projection from a shared midpoint sort and uses
  dynamic rendering.

### vulkan_radix_sort

- Version: `0.6.0`, vendored single header at
  `core/render/vk_radix_sort.h`
- Upstream: <https://github.com/jaesung-cs/vulkan_radix_sort>
- License: MIT, Copyright (c) 2024 jaesung-cs
- Used for: the reduce-then-scan 32-bit radix sort (four 8-bit passes,
  key-value mode, indirect element count) behind `core/render/radix_sort.h`.
  The header is unmodified apart from an attribution comment.

## Ported into the PS5 payloads and titles (source in this repository)

The payload libraries, payloads and titles are GPL-3.0-or-later like this
repository. The notices below are the per-directory `platform/ps5/hmd2/NOTICE`
content, merged here.

### PS5VR

- Upstream: <https://github.com/theghostonline/PS5VR> (the clone used here is
  the `mtp-24` fork, <https://github.com/mtp-24/PS5VR>)
- License: GNU General Public License v3.0 or later
- Notice: Copyright (C) 2026 Husam Osman. PS5VR - Copyright (C) 2026 Husam
  Osman - is distributed under the GNU General Public License v3.0 or later
  (PS5VR's own `THIRD_PARTY_NOTICES.md`).
- Ported into `platform/ps5/hmd2/hmd2.cpp`:
  - `app/src/app_vr.c`: loading libSceHmd2 / libSceVrTracker2 / libSceVrHand
    through `sceSysmoduleLoadModule` + `sceKernelLoadStartModule`, resolving
    their functions by `sceKernelDlsym` or by this firmware's export offsets
    after the module-name and anchor-string check; the Hmd2 initialize / Open /
    FOV / tracker QueryMemory+Initialize+RegisterDevice / reprojection memory
    and Initialize sequence and its parameter blocks; the VideoOut bus-32 port
    (`sceVideoOutOpen(0xff, 32, 0, NULL)`), the buffer attribute, the type 0x10
    layer sets, `sceVideoOutRegisterBuffers2` with category 0x10,
    `sceVideoOutConfigureOutput(0x2000f)`,
    `sceHmd2ReprojectionSetRenderConfig(1)`, the per-frame BeginFrame /
    GetPredictedDisplayTime / SetParam / `sceVideoOutSubmitFlip` sequence and
    the layer layout it fills, the 60 Hz frame cadence, the headset-worn poll
    through `sceHmd2GetDeviceInformation`, and the shutdown order
    (DisableVrMode, buffer unregister with the busy retry, Close, tracker
    UnregisterDevice/Finalize, reprojection Terminate, Hmd2 Close/Terminate).
  - `app/engine/media/src/eng_agc_writer.c`: the GFX10 2D texture-descriptor
    builder (`build_tsharp_2d_internal` and
    `eng_agc_build_tsharp_render_target`'s field placement and tiling bit).
    `hmd2.cpp` builds the descriptor for a B8G8R8A8 surface with the BGRA
    swizzle instead of an RGBA8 AGC target.
  - `app/src/vr_exports_1360.h`, `app/src/vr_exports_761.h`: the export offsets
    and anchor strings, now in `tools/export-tables/tables.json` and emitted as
    `platform/ps5/hmd2/hmd2_exports.h`.
- Modified for this project: the eye buffers are direct memory filled from the
  CPU, not AGC render targets, so the library needs no GPU API.

### HLA-PSVR2

- Upstream: <https://theghostonline.dev/theghostonline/HLA-PSVR2>
- License: GNU General Public License v3.0 or later
- Notice: Copyright (C) 2026 theghostonline. HLA-PSVR2 (the PS5XR runtime and
  the Half-Life: Alyx PS VR2 port) is distributed under the GNU General Public
  License v3.0 or later.
- Ported into `platform/ps5/hmd2/hmd2.cpp` and used by
  `platform/ps5/vr_hello/title.cpp`:
  - `ps5xr/runtime/src/psvr2.c` `tsharp_rgba8`: the linear RGBA8 texture
    descriptor (format `8_8_8_8_UNORM`, X,Y,Z,W swizzle, no tiling, pitch ==
    width) that makes a `vkMapMemory` pointer a sampleable eye texture.
  - `psvr2.c` `wait_display_live`: the three-vblank, 5 s gate after
    EnableVrMode, before the first flip.
  - `psvr2.c` `psvr2_begin_frame` and `xr_session.c` `x_WaitFrame`: the
    predicted-display-time pacing (nap to the next slot from the last
    BeginFrame, then to `display - period - 4160 us - 1500 us` when the app has
    slack), the four-deep begun-frame queue, and the re-submit of a frame two
    or more behind with the last image (`show_last`).
  - `psvr2.c` `submit_flip`: per-eye tangents as the headset's own field of
    view (no overscan) and the `SetParam` + `SubmitFlip` ordering.
  - `xr_vulkan.c` `vk_output_start`/`vk_compose`: three host-visible,
    host-coherent `VkBuffer`s of `2*eye_w*eye_h*4` bytes, mapped and handed to
    Hmd2 by address; one command buffer and fence per buffer, a bounded 500 ms
    fence wait before `SetParam`/`SubmitFlip`, and no CPU cache flush after the
    GPU write.
- Modified for this project: the buffers are filled by this project's own
  Vulkan pass instead of an OpenXR swapchain, and framed by `hmd2`'s API.

### ps5-native-app-boilerplate (via PS5_Vulkan)

- Upstream: <https://github.com/mihawk-99/PS5_Vulkan>, `src/demo_renderer.cpp`
  ("ps5-native-app-boilerplate - CPU VideoOut demonstration implementation")
- License: GNU General Public License v3.0 or later
- Notice: Copyright (C) 2026 BlackBearReloaded, Copyright (C) 2026 Mihawk-99
- Adapted into `platform/ps5/hmd2/hmd2.cpp`: `hmd2::pixel_offset`, the address
  of a pixel in a 64 KiB-tiled (`SW_64KB_R_X`) VideoOut surface - 64 KiB tiles
  of 128x128 pixels, row-major tile index, the in-tile address from the R_X
  swizzle. The formula is unchanged; this copy takes the surface width as an
  argument instead of a file-scope frame width.

PlayStation, PS5 and PS VR2 are trademarks of Sony Interactive Entertainment.
This project is not affiliated with Sony.

## Referenced for documented behaviour (no code copied)

### PS5LM

- Upstream: <https://github.com/cobanov/PS5LM>
- License: GNU General Public License v3.0
- Used as a blueprint only: this project follows PS5LM's *documented* build and
  launch behaviour (the RADV unified-heap setting, the `prof/cpu` and
  `prof/gpu` clocks, the title-link ordering with lld, the ShadowMountPlus
  `/data/homebrew` layout and the HEN app-jailbreak allowlist note). No PS5LM
  source file was copied; the recipes here are written against the public
  documentation and the console's own behaviour.

## Model backends

The SHARP and Depth Anything V2 stage graphs in `core/recon/` are independent
ggml implementations written from the papers and the reference code's
observable behaviour. No upstream model source file is copied, and no
model weights are distributed. See `docs/research/sharp-reference.md` and
`docs/research/op-audit.md` for the provenance notes.

## Used by the PS5 build (not bundled in this repository)

### Mesa / RADV

- Upstream: <https://gitlab.freedesktop.org/mesa/mesa>
- License: MIT
- Used for: the PS5_Vulkan RADV driver port. Built separately in the console
  toolchain and linked directly, without the Vulkan loader.

Minimal patches to third-party sources for the console build will live in
`patches/` and are distributed under the same license as the patched code.

## Not distributed

Apple's SHARP checkpoint and any converted GGUF/`.psplat` derivatives are
fetched and produced on the user's console after accepting Apple's model
license. They are never included in releases. See `docs/spec.md`, "Licensing
and distribution".
