# Photo Splat VR — Technical Specification

Oct 25, 2026

## Overview

Photo Splat VR is a homebrew app for a jailbroken PS5. It turns a single 2D photo into a 3D Gaussian splat scene on the console and lets the user stand inside it with PSVR2. No PC or cloud is involved at runtime.

**Goals**

1. **On-console conversion.** Photo in, Gaussian splat scene out, computed on the PS5 GPU.
2. **Comfortable VR viewing.** Stereo 6DoF viewing in PSVR2 at the headset's refresh rate, using the system reprojection.
3. **Public, easy install.** Ships as a homebrew title folder that installs like other PS5 homebrew apps.
4. **Model-agnostic pipeline.** SHARP is the quality backend, and a permissively licensed fallback backend means the release never depends on one model's terms.

**Non-goals for v1**

- Multi-view capture or training splats (photogrammetry). The input is one photo.
- Room-scale walking. Single-view reconstructions hold up within roughly a head-sized volume around the original camera.
- Video or 4D splats. Possible later, as a frame-by-frame extension.
- Any commercial use, PSN distribution, or bundled model weights.

**Target user.** Someone who owns a jailbroken PS5 on a supported firmware and a PSVR2, and is comfortable copying a folder over FTP.

**v1.0 success criteria (targets, to be confirmed after Spike A)**

| Criterion | Target |
| --- | --- |
| Photo to scene, SHARP backend | 60 s or less per photo on PS5 |
| Viewing frame rate | Stable 90 Hz native, or 60 Hz rendered and reprojected to 120 Hz, with no visible judder |
| Stability | 30-minute session with no crash or memory growth |
| Install | 5 steps or fewer from the release zip |
| First run | Model fetched, converted and verified without a PC |

## Constraints and assumptions

The app runs as a native PS5 title, so it gets the GPU and about 11.4 GiB of unified memory, shared by everything it does.

| Area | Constraint | Basis |
| --- | --- | --- |
| Firmware | 7.00 to 13.60. Test matrix starts with 13.60 and 7.20 | [PS5LM](https://github.com/cobanov/PS5LM/wiki), [PS5VR](https://github.com/tertiumndatur/PS5VR) |
| Payloads | kstuff(-lite), etaHEN, ShadowMountPlus required. elfldr on port 9021 recommended | [PS5LM](https://github.com/cobanov/PS5LM/wiki), [PS5VR](https://github.com/mtp-24/PS5VR) |
| GPU API | Vulkan via the PS5\_Vulkan RADV port, linked directly (no Vulkan loader) | [PS5LM build doc](https://github.com/cobanov/PS5LM/blob/main/docs/BUILDING.md) |
| Memory | One RADV unified heap of about 11.44 GiB for the whole app | [PS5LM build doc](https://github.com/cobanov/PS5LM/blob/main/docs/BUILDING.md) |
| Flat-screen UI | ps5-homebrew-ui over ps5-opengl, in the same process as RADV, with GL symbols hidden to avoid Mesa clashes | [PS5LM Plus](https://github.com/abhi245y/PS5LM-Plus) |
| Headset | PSVR2 through the system libraries libSceHmd2, libSceVrTracker2, libSceVrHand, resolved at runtime by export tables per firmware | [PS5VR](https://github.com/tertiumndatur/PS5VR) |
| Build host | x86-64 Linux for the RADV build (Arch/CachyOS or Fedora). App code can also build on Apple silicon against the payload SDK | [PS5LM build doc](https://github.com/cobanov/PS5LM/blob/main/docs/BUILDING.md), [PS5VR](https://theghostonline.dev/theghostonline/PS5VR/src/branch/main) |
| Runtime | Everything runs on the console. No PC, Mac or cloud at runtime | Project decision |

**Assumptions**

- The user's PSVR2 runs stock firmware and works with the console normally.
- The console can reach the internet once, for the model download. An offline path exists: the user copies the checkpoint over FTP or USB.
- Users accept that this is experimental homebrew with no warranty.
- The app can be installed as a separate title ID, so it can sit alongside PS5LM and PS5VR without conflicts.

## Licensing and distribution

The release ships our code only. SHARP weights are fetched from Apple and converted on each user's console, after the user accepts Apple's license.

Apple's [model license](https://github.com/apple-aiml-research/ml-sharp/blob/main/LICENSE_MODEL) allows use, modification, redistribution and derivatives only for Research Purposes. It defines those as non-commercial scientific research, and it excludes product development and use in any product or service. Redistribution requires the license text, Apple's attribution notice, and derivatives clearly labelled with their changes. A free homebrew viewer is non-commercial but not obviously research, so we treat it as a gray area and keep clear of redistribution. This is not legal advice.

**Rules**

1. **No weights in releases.** No `.pt`, no converted GGUF, and no mirrors of either.
2. **First-run fetch from the official URL** (`ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt`, per the [ml-sharp README](https://github.com/apple-aiml-research/ml-sharp)). The app pins a SHA-256 of the checkpoint and verifies the download against it.
3. **License gate.** Before downloading, the app shows the full LICENSE\_MODEL text and the required attribution notice, and records the user's acceptance locally.
4. **On-console conversion.** The converted runtime file is a model derivative under that license. It stays on the user's console, and the app labels it as converted, listing what changed (dtype and layout).
5. **No Apple branding.** No Apple name or logo in the app's name, icon or promotion. Credits and the paper citation go on an About screen.
6. **Our code license.** GPL-3.0-or-later for the app. Third-party notices for llama.cpp/ggml (MIT), Mesa/RADV (MIT), and the homebrew SDKs; the PS5VR, HLA-PSVR2 and PS5_Vulkan code that was ported is itself GPL-3.0-or-later.
7. **Reimplementation, not copy.** The ggml implementation follows the paper and the reference code's behaviour. Before porting any file closely, check the separate code LICENSE and ACKNOWLEDGEMENTS in ml-sharp.
8. **Fallback backend.** A permissively licensed backend ships enabled by default, so the app works without accepting Apple's terms. SHARP is an opt-in quality upgrade.

## System architecture

&#91;embedded content: system architecture · 10 parts, 2 phases\]

Conversion and viewing take turns on the GPU. The model is unloaded before the renderer allocates, and the scene store is the only hand-off between the two phases. Dashed boxes are outside the app.

## Component specs

Nine components inside one native title. The reconstruction backends are swappable behind one interface, and everything downstream only sees a splat scene.

### C1 Model manager

Handles license acceptance, download, verification and conversion of model weights.

- Shows LICENSE\_MODEL and the attribution notice, then stores the acceptance in `/data/<app>/state/licenses.json`.
- Downloads the official checkpoint over HTTPS with resume support, or picks it up from `/data/<app>/inbox/` (FTP/USB offline path).
- Verifies a pinned SHA-256 before conversion.
- **Converter.** A `.pt` file is a zip archive holding a pickle (`data.pkl`) plus raw tensor blobs. Write a minimal C++ unpickler that handles only the opcodes and `torch._utils._rebuild_tensor_v2` records the checkpoint uses. Stream each tensor to fp16 GGUF via ggml's gguf writer, one tensor at a time, so peak memory stays near the largest single tensor.
- Writes the GGUF with metadata: backend name, source SHA-256, converter version, and the list of changes (dtype and layout).

### C2 Reconstruction interface

```cpp
struct ReconstructionInput { Image rgb; std::optional<float> focal_px; };
class IReconstructor {
  virtual BackendInfo info() const = 0;       // name, license, VRAM need
  virtual Status load(const Path& gguf) = 0;
  virtual Result<SplatScene> run(const ReconstructionInput&, Progress&) = 0;
  virtual void unload() = 0;
};
```

The focal length comes from EXIF when present. Otherwise the backend's own estimate is used, or a default field of view if it has none.

### C3 SHARP backend (ggml reimplementation)

A reimplementation of the [SHARP](https://github.com/apple-aiml-research/ml-sharp) forward pass on ggml-vulkan. Per the paper and its [notes](https://en.papernotes.org/ICLR2026/3d_vision/sharp_monocular_view_synthesis_in_less_than_a_second/), the stages are:

1. **Preprocess.** Decode JPEG/PNG and resize to 3×1536×1536.
2. **Encoder.** Depth Pro's dual ViT backbone.
3. **Depth decoder.** DPT-style, producing two depth layers: visible surfaces, plus a second layer for occlusion fill.
4. **Depth adjustment.** A small U-Net. Confirm from the reference code whether it runs at inference or only in training.
5. **Gaussian decoder.** Predicts residuals for position, scale, rotation, colour and opacity.
6. **Initialiser/composer.** Unprojects with the intrinsics and assembles roughly 1.2 million Gaussians.

The output is metric scale, in OpenCV axes (x right, y down, z forward), with the scene centre near (0, 0, +z), per the [README](https://github.com/apple-aiml-research/ml-sharp).

Implementation work items:

- Audit every op against ggml-vulkan support: conv2d, transposed conv, bilinear resize, layer norm, attention at ViT-L scale. Write any missing op as a ggml custom op with a Vulkan shader.
- Process the encoder's patches in batches, so activation memory stays bounded.
- Write the composer's output straight into the renderer's buffer layout, avoiding an extra copy.

### C4 Fallback backend

Uses [Depth Anything V2 Small](https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf) (24.8M parameters). It is the only V2 size under Apache-2.0; Base and Large are CC-BY-NC-4.0, per the [project README](https://replicate.com/chenxwh/depth-anything-v2).

- Single depth layer, relative scale. Map to metric with a fixed assumed scene depth, adjustable by the user in VR.
- One Gaussian per pixel at a reduced grid, about 0.5 million. Scale comes from pixel footprint at depth, and the colour is the pixel colour.
- At depth discontinuities, reduce opacity or drop Gaussians. This avoids the stretched rubber-sheet look.
- Weights may be bundled with attribution. Even so, fetch them the same way as SHARP's, to keep a single code path.

### C5 Scene store

- One folder per photo: `original.jpg`, `thumb.jpg`, `scene.psplat`, `meta.json` (backend, intrinsics, time taken, version).
- `.psplat` is a header followed by struct-of-arrays fp16 attributes: position, log-scale, rotation quaternion, RGB (SH degree 0), opacity. At 1.2 million Gaussians and 14 values each, that is about 34 MB, versus about 67 MB in fp32.
- Import and export standard 3DGS `.ply`, so scenes made elsewhere also play.

### C6 Splat renderer

Vulkan, built on RADV. Start from [vkgs](https://awesome.ecosyste.ms/projects/9947868) (MIT): it already does frustum culling, indirect sort and draw, and its own radix sort.

Per frame:

1. Cull and project (compute).
2. Build 32-bit depth keys (compute).
3. Radix sort (compute).
4. Draw instanced, alpha-blended quads, once per eye.

Design choices:

- **Sort once per frame, from the midpoint between the eyes.** Both eyes share that order. The ordering error from the eye offset is negligible at these distances.
- **Use reduce-then-scan radix sort as the default.** vkgs reports its Onesweep path failing on macOS, so it is the safer cross-driver choice. Try Onesweep as an option on RDNA2.
- **Configurable eye-buffer resolution.** Start below the panel resolution and tune. Foveated rendering using PSVR2 eye tracking is a v2 item.

### C7 PSVR2 display layer

Copies PS5VR's approach: load libSceHmd2 / libSceVrTracker2 / libSceVrHand at runtime through per-firmware export tables. Each frame, get the predicted pose, render, and submit the eye buffers as a layer through Hmd2's reprojection, as [PS5VR](https://github.com/tertiumndatur/PS5VR) does.

- **Buffer interop** between RADV images and what Hmd2 accepts is the key unknown. Spike B resolves it.
- **Comfort boundary.** When the head moves beyond a configurable radius from the capture point, fade toward a neutral backdrop instead of showing torn geometry.
- **Controls.** Recentre on R3. Auto-pause when the headset is removed.

### C8 UI and input

- **Flat screen** (ps5-homebrew-ui): gallery, conversion queue with progress, settings, licenses, About.
- **In VR:** a minimal panel. DualSense L1/R1 for previous/next scene, the stick for world scale, R3 to recentre.

### C9 Photo ingest and jobs

- **LAN web page** on a configurable port. A phone uploads photos after entering a PIN shown on the TV. No internet exposure.
- **Inbox watcher** on `/data/<app>/inbox/` and attached USB drives.
- **Job queue.** One conversion at a time, persisted across restarts. Inference and VR viewing are exclusive in v1: the model is unloaded before the renderer allocates.
- **Logging** to klogsrv. Read temperature and power via ps5-exporter when it is present.

## Performance and memory budgets

Inference and viewing never share memory in v1, so each phase gets the full heap minus a fixed reserve. Every figure below is a target or estimate until measured in Spikes A and B.

**Memory (unified heap of about 11.44 GiB)**

| Phase | Budget | Notes |
| --- | --- | --- |
| Reserve: UI, network, decode, driver | 1.0 GiB | Always held |
| Conversion (.pt to GGUF) | Largest single tensor + 256 MiB | Streaming, one tensor at a time |
| SHARP inference | 8 GiB or less | The reference reports about 4–8 GB on GPU ([HF Space](https://huggingface.co/spaces/YUGOROU/ml-sharp_ZeroGPU/blob/main/README.md)) |
| Fallback inference | 1 GiB or less | 24.8M-parameter model |
| Viewing one scene | 512 MiB or less | Scene of about 34 MB fp16, plus projection and sort buffers and eye buffers |
| Preloading next scene | +64 MiB | Smooth previous/next in VR |

**Time**

| Metric | Target | Stretch |
| --- | --- | --- |
| SHARP, photo to scene | 60 s or less | 15 s or less |
| Fallback, photo to scene | 5 s or less | 2 s or less |
| GPU frame, both eyes, 60 Hz reprojected to 120 Hz | 16.6 ms or less | — |
| GPU frame, both eyes, 90 Hz native | — | 11.1 ms or less |
| Of which: cull + project + sort (1.2 million splats) | 4 ms or less | 2.5 ms or less |
| Scene load from SSD to first VR frame | 1 s or less | 0.3 s or less |

**Thermal and stability.** Run 20 back-to-back conversions, then 30 minutes in VR, with no throttling-related failures and no memory growth. Log temperature through ps5-exporter during the run.

## Development environment, repo layout and testing

The same core code runs on a desktop and on the PS5. Only a thin platform layer differs, so most debugging happens on a PC, against golden outputs from the PyTorch reference.

**Environments**

| Environment | Purpose |
| --- | --- |
| Python + ml-sharp reference (any machine) | Produces golden tensors and reference `.ply` files |
| Desktop Linux x86-64, ideally an AMD GPU on RADV | Port development, closest match to the console's driver |
| macOS via MoltenVK (optional) | Convenience builds of core and renderer only. Not a parity target |
| PS5 toolchain in Docker (x86-64 Linux) | PS5\_Vulkan RADV build, payload SDK, title packaging |
| PS5 on 13.60 (+ 7.20 later) | Integration, VR, performance |

**Repo layout**

```text
ps5-splat-vr/
  core/
    model/        # C1 manager, .pt unpickler, GGUF writer
    recon/        # C2 interface, C3 sharp/, C4 fallback/
    scene/        # C5 .psplat + .ply IO
    render/       # C6 Vulkan renderer, shaders/, sort/
    jobs/         # C9 queue, inbox watcher
  platform/
    desktop/      # GLFW window, stereo preview, file dialogs
    ps5/          # title entry, RADV init, C7 hmd2/, C8 ui/, ingest http
  tools/
    golden/       # PyTorch hooks to dump per-stage tensors (.npy)
    parity/       # compares ggml outputs with golden tensors
    export-tables/  # per-firmware Hmd2 export offsets
  third_party/    # ggml (submodule), vkgs-derived sort, stb, miniz
  patches/        # minimal patches to ggml / mesa
  docs/
```

**Testing**

- **Stage parity.** Hook every SHARP stage in PyTorch and dump inputs and outputs for about 10 fixed photos. In fp32, ggml must match each stage within 1e-3 maximum relative error. In fp16, cosine similarity must be at least 0.999 per tensor.
- **End-to-end image parity.** Render the reference `.ply` and the reimplementation's `.psplat` with our renderer from 8 fixed poses within the comfort radius. PSNR must be at least 35 dB against the reference renders.
- **Unit tests.** Unpickler on real and synthetic checkpoints; `.psplat` round trip; sort correctness on random keys including ties; EXIF focal parsing.
- **On-console soak.** The thermal and stability run from the budgets section, automated through elfldr and a test mode in the app.
- **CI.** GitHub Actions builds the desktop target with unit and parity tests, plus the PS5 title in the Docker toolchain. Parity runs on CPU ggml in CI, since CI has no GPU.

## Milestones and gates

&#91;embedded content: milestones · 2 spikes, 3 build phases, 5 gates\]

Spikes A and B are independent, so they can run in parallel. Nothing from M3 on starts until both gates pass. No dates are set yet: estimate M3 to M5 once Spike A gives a real inference time on the console.

## Risks, open questions and references

The two risks that can sink the project are display interop and inference feasibility. Both are tested before any product work starts.

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Hmd2 rejects RADV-allocated images | No VR output from our renderer | Spike B first. Fallbacks: share memory between the two allocators, a GPU copy into an Hmd2-owned buffer, or rendering with the API PS5VR uses |
| ggml-vulkan lacks ops, or fp16 drifts | Port stalls or quality loss | Op audit in week 1. Custom ops with Vulkan shaders. Keep numerically sensitive layers in fp32 |
| SHARP too slow or too large on PS5 | Misses the 60 s target, or runs out of memory | Batched encoder patches. Fewer second-layer Gaussians. Lower internal resolution as a quality setting |
| Long GPU submissions hit a timeout (device lost) | Crashes on big graphs | Split the graph into several submits. RADV device-lost on long compute jobs is a known llama.cpp issue ([#21724](https://github.com/ggml-org/llama.cpp/issues/21724)) |
| Mesa symbol clashes between GL (UI) and RADV | Link or runtime failures | Reuse PS5LM Plus's approach of linking GL with only its API visible ([PS5LM Plus](https://github.com/abhi245y/PS5LM-Plus)) |
| Per-firmware export tables for the VR libraries | Some firmwares unsupported | Ship 13.60 and 7.20 first. Generate tables with tooling like PS5VR's `gen_export_table.py` |
| Apple changes the license or objects | SHARP backend must be removed | Weights are never hosted. The fallback backend keeps the app working. Respond to takedowns within a day |
| Discomfort from artifacts outside the valid volume | Bad VR experience | Comfort-boundary fade, seated default, adjustable world scale |
| Upstream drift (ggml, Mesa port, payloads) | Broken builds | Pin everything as submodules. Keep patches small and documented |

**Open questions**

- [ ] Does SHARP's depth adjustment U-Net run at inference, or only in training?
- [ ] SHARP checkpoint size, parameter count and SH degree of the output colours.
- [ ] How SHARP obtains focal length when the photo has no EXIF data.
- [ ] Which GPU API does PS5VR use to create the buffers it submits to Hmd2?
- [ ] Eye-buffer sizes and refresh modes (90/120 Hz) that Hmd2 accepts from a homebrew title.
- [ ] Can a homebrew title read the PS5's own screenshot/capture folder?
- [ ] PS5VR's code license, before reusing its VR bindings directly.
- [ ] Final project name, title ID and icon.

**References**

- [apple-aiml-research/ml-sharp](https://github.com/apple-aiml-research/ml-sharp) and its [LICENSE\_MODEL](https://github.com/apple-aiml-research/ml-sharp/blob/main/LICENSE_MODEL)
- [SHARP paper notes (ICLR 2026)](https://en.papernotes.org/ICLR2026/3d_vision/sharp_monocular_view_synthesis_in_less_than_a_second/)
- [PS5LM wiki](https://github.com/cobanov/PS5LM/wiki) and [build doc](https://github.com/cobanov/PS5LM/blob/main/docs/BUILDING.md)
- [PS5LM Plus](https://github.com/abhi245y/PS5LM-Plus)
- [PS5VR](https://github.com/mtp-24/PS5VR) and its [fork with implementation notes](https://github.com/tertiumndatur/PS5VR)
- [vkgs Vulkan splat viewer](https://awesome.ecosyste.ms/projects/9947868)
- [Depth Anything V2 Small](https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf)
- [llama.cpp issue #21724, RADV device lost](https://github.com/ggml-org/llama.cpp/issues/21724)
