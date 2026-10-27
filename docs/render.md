# C6 splat renderer

Vulkan 1.3 renderer for `.psplat` scenes and the desktop viewer. The desktop
target is the Mac/MoltenVK convenience build; the PS5 build uses the same
renderer sources against RADV.

## Pipeline

One frame, per `SplatRenderer::render()`:

1. **Upload (once).** `SplatScene` is converted to fp16 and copied into five
   struct-of-arrays storage buffers in the `.psplat` order: position (3),
   log-scale (3), rotation (4, w x y z), SH DC (3) and opacity logit (1).
   Conversion uses `float_to_half` from `core/common/half.h`, so the GPU
   buffers are byte-identical to the `.psplat` payload. Buffers are
   host-visible and coherent: desktop and PS5 have unified memory, and the
   scene is uploaded once.
2. **Project (compute, 256 threads).** For each splat: transform by the
   world-to-camera matrix, cull behind the near plane or outside the viewport,
   build the 3D covariance `R S Sᵀ Rᵀ`, project it with the EWA Jacobian
   `J = [[fx/z, 0, -fx·x/z²], [0, fy/z, -fy·y/z²]]` as `J Σ Jᵀ`,
   eigendecompose to the 2D conic axes, evaluate SH degree 0
   (`dc · C0 + 0.5`, clamped) and `sigmoid(opacity)`.
   Every splat writes its key, its own index and its instance at **slot ==
   splat id** (not an atomic compaction slot); culled splats write the largest
   key `0xffffffff` and no instance. Keys are `~floatBitsToUint(view_z)`, so an
   ascending sort draws far to near. Instances store the midpoint view
   position, the conic axes in NDC and RGBA. The atomic counter only counts,
   and that count is the draw instance count.

   GLSL's `mat3`/`mat2` constructors fill **columns**, so both matrices are
   built from their rows and wrapped in `transpose()`. Passing the rows
   directly (the bug fixed in review F1) builds `Rᵀ` and `Jᵀ`: the covariance
   came out as `Rᵀ S Sᵀ R`, and `mat2(Jᵀ Σ J)` discarded the two depth-gradient
   terms entirely, which flattened every off-axis splat. `core/render/
   project_math.h` is the CPU twin of this maths and the tests compare the two.

   **Low-pass filter (anti-aliasing).** `project.comp` adds
   `RendererOptions::low_pass_eps` to the 2D covariance diagonal and, with
   `low_pass_compensate`, scales the splat's alpha by
   `sqrt(det(Σ)/det(Σ+eps))` so the dilation does not brighten it. The default
   is **0.3 px² with compensation** — gsplat's `eps2d` / 3DGS's `cov2d += 0.3`
   with Mip-Splatting's 2D-filter opacity compensation. The reason is the
   headset eye: see "Anti-aliasing (AA)" below.

   SHARP's own renderer (`ref/ml-sharp/src/sharp/utils/gsplat.py`) calls
   `gsplat.rendering.rasterization(..., rasterize_mode="classic",
   eps2d=self.low_pass_filter_eps)` and constructs `GSplatRenderer` without
   overriding `low_pass_filter_eps`, i.e. **eps2d = 0.0, classic mode, no
   opacity compensation**. That is the right convention for its stills at the
   capture resolution, where the splats are wider than a pixel, and it is what
   the projection tests pin (`tests/render_projection_test.cpp` sets
   `low_pass_eps = 0`): the conic maths and `project_math.h` are compared at
   eps = 0. (F1's bug was a transposed rotation/Jacobian, not the filter. The
   0.3 that was used before F1 happened to hide that bug; the covariance fix,
   not the filter value, is what removed the extra blur.)

   **Cull radius.** `splat.vert` puts the quad corners at
   `ndc + 3·(±axis0 ±axis1)`, so the quad reaches `3·(|axis0| + |axis1|)` from
   its centre on each NDC axis. The projection pass culls with exactly that
   reach; the previous `3·max(|axis0|, |axis1|)` (clamped to 8, which in NDC
   units is no clamp at all) dropped quads whose corner still crossed the
   border, so splats popped along the viewport edges. On a stereo frame the cull
   also adds the per-eye x shift (`|fx·half IPD/z|·2/width`), because the sort
   and instances are shared but the draw moves each eye by half the IPD; see
   "Per-eye cull margin" under the comfort boundary.

   **Screen-radius cap.** A splat whose centre is just past the near plane
   projects to a conic thousands of pixels wide; left alone it can paint a
   whole eye, and a few thousand of them cost 180-245 ms of overdraw (the VR2
   headset run, with the camera inside the cloud, logged 5k-15k visible splats
   and that draw time). `project.comp` now clamps the 3σ screen radius to
   **512 px** before converting the conic to NDC. At the 1984×2048 PSVR2 eye
   that is a 1024 px quad, half the width and about a quarter of the area, so
   one splat can never cover the eye. Normal splats are far below the cap and
   byte-identical; the existing comfort, projection and determinism renders are
   unchanged. The cap shrinks the quad, so a capped splat's Gaussian is
   evaluated over a smaller disc; its colour and centre stay where the
   projection puts them. On `immichvr-1` at 1984×2048 the cap never fires: the
   largest 3σ reach at the capture pose is **29.4 px**, so 0 of 1,179,631
   visible splats are capped (measured, and cap 256 / cap off render the same
   frames to the byte — see "Anti-aliasing"). It is a guard against a camera
   inside the cloud, not a shaper of normal frames.
3. **Finalize (compute, 1 thread).** Copies the visible count into a
   `VkDrawIndirectCommand` (4-vertex triangle strip, one instance per splat).
4. **Sort (compute).** The vendored `vulkan_radix_sort` 0.6.0 reduce-then-scan
   radix sort in key-value mode, with the element count read from the second
   uint of the counters buffer (indirect sort). It sorts the whole splat array,
   culled keys included, so the first `visible_count` sorted entries are
   exactly the visible splats. Four 8-bit passes, no Onesweep. Sorting 1.18 M
   entries instead of ~1.1 M costs about 0.1 ms.
5. **Draw (graphics, indirect).** One `vkCmdDrawIndirect` per eye into one
   side-by-side image. The vertex shader gathers `values[gl_InstanceIndex]`,
   shifts the splat by the eye offset along camera x, projects the centre and
   emits a 3σ quad with the sorted order; the fragment shader applies the
   Gaussian alpha and premultiplied alpha blending. The same draw also writes
   the splat's depth into a D32 attachment with the depth test set to ALWAYS and
   depth writes on: because the order is far to near, the last write is the
   nearest splat. The comfort pass samples that buffer (see below); the
   attachment is cleared every frame and costs no measurable time at 1280x873
   (draw 8.53 ms with it, 8.72 ms in the previous run).

The scene is sorted **once per frame from the midpoint between the eyes**.
Both eyes share the same order and instance count. Eye positions are
midpoint ±IPD/2 along the camera right axis; only the projected centre changes
per eye, the conic axes and colour come from the midpoint projection.

**Colour space.** The SH DC values in `.psplat`/`.ply` are sRGB-encoded, and
3DGS composites in linear light: ml-sharp converts them with
`sRGB2linearRGB` when it loads a `.ply`, blends, and converts the result back
with `linearRGB2sRGB`. The renderer does the same by rendering into an **sRGB**
target: the fragment shader decodes with the exact sRGB EOTF
(`core/render/shaders/splat.frag`), the fixed-function blend unit works on
linear values, and the store re-encodes. The desktop prefers an sRGB swapchain
format for the same reason and only falls back to UNORM (encoded-space
blending, printed as a warning) when the surface offers nothing else.

The other two suspects the review listed are clean: the blend factors are
`ONE` / `ONE_MINUS_SRC_ALPHA` with the fragment shader already emitting
premultiplied `rgb·a`, and storing the SH DC values as fp16 moves a colour by
at most half an fp16 ulp (~1.4e-4 in [0,1], a twentieth of an 8-bit level).
The measured ~1 % came from the transfer function, not from those.

Blending encoded values darkens the mid-tones: on the same scene the
encoded-space blend of the independent numpy renderer gives image means
0.7485/0.6192/0.5272 where the linear-light blend gives 0.7547/0.6275/0.5366 —
about 1 % darker. The pre-fix GPU pipeline did exactly that (UNORM target, no
linearisation) and measured 0.7422/0.6087/0.5161; with both defects fixed it is
0.7520/0.6233/0.5322.

## Anti-aliasing (AA)

The PSVR2 user report: *"when I move my head some edges blink; a bit of
anti-aliasing could make it better"* (1984×2048 per eye, 60 fps rendered,
120 Hz reprojected). Reproduced on the desktop on `runs/sharp_e2e/immichvr-1`
(SHARP, 1,179,648 splats) with the headset eye projection from
`tests/vr_view_test.cpp` (the Hmd2 tangents 1.842/0.947 and 1.329/1.329 rad):
fx 711 px at 1984×2048 and a ±318 px per-eye principal-point shift. The scene's capture intrinsics are fx 1664 px at
1920×1440, so **the headset eye's pixel scale is 0.43× the capture's**: the same
splats project 2.3× smaller. At the capture pose, 84 % of the visible splats
have a projected 2D σ below **1 px** and 40 % below 0.5 px (median 0.55 px),
against 26 % / 0.01 % at the capture resolution (median 1.23 px). Point-sampling
Gaussians that narrow on a moving grid is exactly the reported blinking: a
sub-pixel splat is a bright dot that jumps between pixels, and an edge made of
them shimmers.

**Metric.** `tests/render_flicker_test.cpp` renders a 7-frame headset-eye
sequence at constant, sub-pixel velocity (0.05° yaw + 0.02° pitch + 1 mm
sideways per frame; 0.62 px at fx 711) and measures the **temporal second
difference** on a 600×590 crop inside the capture photo of both eyes:

```
J = mean |I(k+1) - 2 I(k) + I(k-1)|
```

With constant velocity a band-limited render changes linearly in time, so J is
0 for an ideal sequence; aliasing shows up as pixels popping in and out. `edge`
is the same over the top 10 % of gradient pixels (the blinking edges), `pops` is
the share of pixels that jump more than 8 levels in one step, `grad` the mean
spatial gradient (sharpness). A no-motion control row measures exactly 0.00
everywhere, so the metric has no noise floor of its own.

**Measured variants** (one process, same poses, 1984×2048 per eye, both eyes;
GPU ms are the last six frames' means):

| Variant | J | edge J | pops % | grad | still PSNR vs before | draw ms |
| --- | --- | --- | --- | --- | --- | --- |
| eps2d 0 (before) | 10.57 | 31.84 | 33.2 | 11.1 | — | 21.17 |
| **eps2d 0.3 + compensation (now)** | **3.60** | **10.38** | **12.7** | 7.6 | 38.5 dB | 20.28 |
| eps2d 0.3, no compensation | 5.17 | 15.11 | 19.8 | 9.0 | 35.0 dB | 20.31 |
| eps2d 0.1 + compensation | 6.53 | 19.36 | 24.5 | 9.2 | 43.5 dB | 19.36 |
| eps2d 0.5 + compensation | 2.55 | 7.07 | 7.0 | 6.7 | 36.8 dB | 20.67 |
| eps2d 1.0 + compensation | 1.69 | 4.14 | 2.2 | 5.5 | 35.2 dB | 22.45 |
| eps2d 0.3, hard near cull | 3.60 | 10.38 | 12.7 | 7.6 | same | 20.68 |
| eps2d 0.3, UNORM target | 3.65 | 10.68 | 12.9 | 7.7 | 37.4 dB | 20.33 |
| eps2d 0.3, no motion | 0.00 | 0.00 | 0.0 | 7.6 | same | — |

So the filter cuts the edge flicker by **3.1×** and the popping pixels by
**2.6×** for a still frame that is 38.5 dB from the old one (3 levels RMS out of
255, which is smooth shading on this content, not a visible blur). The compensation is
not optional: without it the same dilation brightens and flickers more (15.1 vs
10.4 edge J, 35.0 dB). `immichvr-1` has no splat nearer than 12 cm at these
poses and none over the 512 px cap, so the near-cull and cap rows are identical
by construction; they were measured, and they cannot be the cause here.

**Not the cause** (each measured with a temporary knob, then reverted, because
the frame is unchanged): the **512 px cap** (largest 3σ reach 29.4 px at this
pose; cap 256 and cap off are byte-identical and byte-identical to 512);
the **0.05 m near cull** (0 splats inside 12 cm, identical with and without the
new fade); the **alpha cut-off** (per-splat 1/255 → 1/32: identical; fragment
1/512 → 1/4096: identical), and with it the **3σ quad truncation**: a 4σ quad
with the cut-off at 1/4096 is 70.0 dB from the 3σ frame, i.e. invisible, and
costs 2.2 ms more fill, so the quad stays at 3σ; **sort stability** (a ±0.25 mm
per-splat key jitter is 61.1 dB from the unjittered frame and changes J by
0.02, so re-ordering near-coplanar splats is invisible; the order is
deterministic for a given pose anyway, see "Determinism"); and **blending
precision / colour space** (an RGBA8 UNORM target blends in encoded space and is
0.3 dB and 3 % worse in edge J than the sRGB target, which blends in linear
light — the current choice).

**The price, measured against the source photo.** Rendering the capture pose at
the capture intrinsics (1920×1440) and comparing with `original.jpg` gives
29.38 dB at eps2d 0, 28.99 dB at 0.1, **28.13 dB at 0.3**, 27.42 dB at 0.5 and
26.23 dB at 1.0. The filter does not improve the still: SHARP fitted these
Gaussians to gsplat's *unfiltered* rasteriser at that resolution, where the
median splat is 1.2 px, so a display prefilter moves the image slightly away
from the photo. 0.3 is the largest value that keeps the still within 35 dB of
the old render and 1.5 dB of the photo, while removing two thirds of the
headset flicker; 0.5 and 1.0 halve the remaining flicker again but visibly
soften everything (grad 6.7 and 5.5) and cost fill time. MSAA and supersampling
were not needed and are not used.

**Near fade.** The 5 cm near cull was a hard on/off: a splat crossing the plane
disappeared in one frame. `project.comp` now ramps alpha up from the near plane
to `RendererOptions::near_fade_end` (**0.08 m**, a 3 cm band, three lines of
shader). `tests/render_flicker_test.cpp` pins the ramp with one splat at 4.9,
5.2, 6, 7 and 10 cm: with the hard cull the step between 4.9 and 5.2 cm is the
whole splat, with the fade the alpha through the band is 0.02 / 0.26 / 0.74 /
1.00 of full (smoothstep), so no depth in the band jumps. On `immichvr-1` the
fade changes nothing (no splat is that close), so it is a guard for leaning
into geometry, not part of the measured flicker fix.

**Cost.** Back-to-back in the same process (1984×2048 per eye, both eyes),
project 1.05 → 0.97 ms, sort 1.54 → 1.53 ms, draw 21.17 → 20.28 ms, total
23.76 → 22.78 ms. The filter is about a dozen ALU ops per splat in the
projection pass, no extra pass, no extra bandwidth. Scaling to the console: the
same scene and size measured **11.29 ms** per frame on the PS5 (project 0.28,
sort 0.39, draw 10.62, `docs/console-runbook.md`), so the console is 2.1× faster
than the desktop's in-process 23.6 ms (the `--bench` path is not the same
comparison: it measured 31.5 ms total, 2.8×). The 8 ms budget is
therefore about 17–23 ms of desktop time — the renderer is already 3 ms over it
at this scene and size, and none of that is this change: the AA's desktop delta
(+0.08 ms project, −0.9 ms draw, both inside the ±0.3 ms run-to-run noise) is
below 0.1 ms on the console.

Crops and flicker maps: `$SPLAT_DATA_DIR/runs/aa/` (`aa_before_after_crop.png`,
`aa_before_after_flicker.png`; the flicker maps are the peak second difference
over the sequence, 4× gain).

## Comfort boundary, recentre and world scale

`core/render/viewer_state.{h,cpp}` is the platform-neutral seated-VR state. The
desktop viewer and the PS5 HMD layer feed it a tracked head pose and get a
`FrameView` back, so the same comfort rules apply on both.

- **Capture-point anchor.** `ViewerState` holds an anchor position in tracking
  space. The scene origin (the capture camera) sits at the anchor, and the
  comfort radius is the head's distance from it.
- **Recentre (R3).** `recentre(head_position, head_yaw)` moves the anchor to
  the current head position and sets the yaw offset so the current head yaw is
  scene yaw zero. Pitch is left alone. The scene-relative position is the head
  offset rotated by `-yaw_offset`, so moving forward after recentring maps to
  scene +z and strafing right maps to scene +x.
- **Headset placement (`HmdView`).** `core/render/hmd_view.{h,cpp}` wraps that
  state for PSVR2: it maps Hmd2's tracker poses (x,y,z,w; +y up; −z forward;
  metres) to the scene axes, takes the eye midpoint and IPD from the per-eye
  poses, and `place()`/`recentre_here()` also make the head's current pitch the
  scene's level, so the first frame (and every R3) is the capture photo upright
  and in front. `ViewerState` itself still leaves pitch alone; the offset lives
  in `HmdView`.
- **World scale.** Clamped to 0.25x–4x. The renderer multiplies both the splat
  positions and the Gaussian standard deviations by the scale
  (`project.comp`), which scales the whole scene about the capture point. At
  the capture pose the image is therefore unchanged by scale; scaling becomes
  visible through parallax when the head leaves the capture point. The
  screenshots below confirm this: at the capture pose scale 0.5 and 2.0 give
  the same frame as 1.0 byte for byte, while at a 10 cm sideways offset they
  differ by 12.5 dB. Scaling positions without the splat sizes
  makes a scaled-up world grainy, so both are scaled.
- **Comfort fade.** Radius defaults to 0.25 m and is configurable
  (`--comfort-radius`). The fade starts at **50 %** of the radius (default,
  `--fade-start`; `kDefaultFadeStart`) and reaches the neutral backdrop at the
  radius: `fade = 1 - smoothstep((d - 0.5r) / 0.5r)`, clamped to [0, 1]. At
  50 % the fade is still 1, at 60 % it is 0.896, at 75 % 0.5, at 90 % 0.104,
  at 100 % 0 and at 120 % 0. The previous 80 % start faded over only 5 cm of
  head travel, which read as an abrupt grey wash; 50 % gives 12.5 cm and the
  smoothstep keeps both ends soft. The curve is a function of distance only, so
  it is frame-rate independent (no per-frame step).

- **Capture-frustum fade (per eye, per pixel).** Even inside the comfort radius the
  view can look past the edge of the original photo, where the Gaussian scene
  only extrapolates: those splats are stretched and smeared (a ~100 px band at
  the right edge at 60 % of the radius). The splat pass stores the nearest splat
  depth per eye (each eye's viewport writes its own half of the depth
  attachment); with the capture intrinsics, the image size and **the eye's own
  position** in the camera UBO, `comfort.frag` turns each pixel's view ray back
  into a scene point at that depth, projects it through the capture camera at
  the scene origin, and fades it toward the backdrop as it leaves the capture
  image:

  ```
  scene      = R^T * (ray * view_z + (eye_offset, 0, 0)) + midpoint
  capture_px = K_capture * scene.xy / scene.z + c_capture
  outside    = max(-u, u - 1, -v, v - 1)               // u, v in image units
  frustum    = 1 - smoothstep(-inner, margin, outside)  // 0.02, 0.03125
  ```

  `eye_offset` is `-half IPD` for the left eye and `+half IPD` for the right
  (`camera_position.w`, zero on a mono frame), applied along camera x before
  the rotation to scene space. The depth only shifts in x between the eyes, so
  `view_z` is the same. Using the eye's own centre, not the midpoint, puts the
  fade edge on the scene surface: the boundary is a fixed world point, so it
  carries the same disparity as the geometry behind it. A point that lands
  behind the capture camera is fully outside. The check uses the *hit point*,
  not just the ray direction: with pure sideways translation the rays keep the
  capture camera's directions, so a direction-only test would never fire, while
  the hit point is exactly the wall region the photo never saw. Pixels with no
  splat (cleared depth) are left alone, they are backdrop already. Rendering
  with no capture intrinsics (`capture_width == 0`) disables the pass term.

  Both ends of the ramp are **fractions of the capture image size**, so the band
  keeps its relative width at any resolution: the outer end
  (`RendererOptions::capture_margin`, 0.03125 = **40 px past the border at 1280
  wide**) and the inner start (`capture_inner_margin`, 0.02 = **25.6 px inside**
  it, clamped to the outer margin so the smoothstep edges stay ordered).
  Starting the ramp inside the photo fixes the strip that SHARP's weak edge
  Gaussians leave at full opacity: 20–25 px at 1280 wide, washed out and
  speckled, sitting bright against the backdrop in both eyes. Those border
  splats now fade with the extrapolated region instead of reaching the border at
  full brightness, and the fully opaque limit moves from the border to 2 % inside
  it. The whole transition is 65.6 px at 1280 wide. At the capture pose the
  photo loses only that 2 % ring: only columns 0–22 and 1256–1279 (plus the
  top/bottom ring) change, the rest of the frame is byte-identical to the
  pre-F7 render, and the capture-pose PSNR against the input photo is unchanged
  (23.21 dB).

  Measured on the 60 % stereo frame (head x = 0.15 m, 1280x873 per eye): the
  fade end (fully faded) sits at x = 1241.5 in the left eye and 1202.5 in the
  right, **39.0 ± 1.1 px of disparity**. With the inner ramp the *start* of the
  fade sits at x = 1185 (left) and 1148 (right), **37 px apart**, so the wider
  band still carries the scene disparity. Cross-correlating the wall 100–160 px
  inside the fade gives the scene disparity there: **38 px** (37 px 180 px
  inside); the 50 % fade crossing differs by 38.6 px. The pre-fix midpoint
  version put both edges at x ≈ 1210, a 1.9 px disparity, i.e. locked to the
  screen and floating at infinity behind the wall.

  **Per-eye cull margin.** `project.comp` culls with the 3σ quad reach around
  the *midpoint* projection, but the draw shifts each eye by ±IPD/2 along camera
  x. A splat whose midpoint centre is just outside the left border can still
  cover the left eye's first pixels; culled, the left eye filled that strip with
  stretched neighbours (a smooth, unfaded smear the right eye did not show).
  The cull now adds the per-eye screen shift `|fx · half IPD / z| · 2/width` to
  the x reach. On the 60 % stereo frame the horizontal gradient energy in the
  left eye's first 2 px / 5 px went from 0.46 / 0.62 (a smooth band) to
  6.7 / 8.8, against 9.6 in the interior of the same eye.

The fade is a **final full-target blend**, not per-splat alpha scaling. After
the splat pass, `comfort.vert`/`comfort.frag` draw one triangle over the target
with premultiplied alpha blending (`ONE`, `ONE_MINUS_SRC_ALPHA`):

```
dst' = backdrop * (1 - fade) + dst * fade
```

where `fade` is the product of the radial and capture-frustum terms. This is
what actually works for a dense scene: scaling each splat's alpha leaves the
accumulated coverage saturated, so the backdrop barely shows. The backdrop is
linear RGB (0.18 grey by default, `RendererOptions::backdrop`) and is encoded
for a UNORM target, matching the renderer's encoded-space fallback. No splat is
dropped, moved or culled by the comfort boundary.

`ViewerState::paused()` carries the auto-pause flag (headset removed, window
unfocused). The desktop toggles it with P; the PS5 HMD layer sets it when
Hmd2 reports the headset is off.

## Conventions

- Scene axes are OpenCV: x right, y down, z forward. The capture camera is at
  the origin looking down +z. There is no axis flip.
- `yaw` rotates about world +y (positive turns right), `pitch` about the
  camera x axis (positive looks down). The view matrix is
  `[s; d; f]` rows with `s = normalize(cross(f, worldUp))` and
  `d = cross(f, s)`, which gives camera (right, down, forward).
- Intrinsics are pinhole pixels: `px = fx·x/z + cx`, `py = fy·y/z + cy`,
  then NDC `2·pixels/size − 1`. Vulkan NDC y points down, matching OpenCV y
  down.
- The default view uses the scene's capture intrinsics when available:
  `meta.json` next to a `.psplat`, the `.psplat` header focal/size, or the
  `intrinsic`/`image_size` elements SHARP writes into a `.ply`. Intrinsics are
  scaled when the render size differs from the capture image. Without any
  intrinsics the default is a 60° horizontal field of view.
- Depth key uses the midpoint view z; near/far are 0.05 m / 1000 m. The near
  plane used to be 0.02 m; anything inside 5 cm is in front of the viewer's
  face, and culling it keeps a splat from filling the eye with a projection of
  itself. `kNearPlane` in `core/render/splat_renderer.cpp`. Alpha ramps up from
  the plane to `RendererOptions::near_fade_end` (0.08 m) so a splat crossing it
  fades instead of popping; see "Anti-aliasing (AA)".

## Desktop viewer

```
splat_desktop <scene.psplat|.ply> [options]
splat_desktop --scenes DIR [options]
  --size WxH              eye resolution (default: capture image size, else 1280x720)
  --stereo                side-by-side stereo, IPD 63 mm
  --screenshot out.png    render one offscreen frame and exit
  --pose x,y,z,yaw,pitch  head pose, metres and degrees (default 0,0,0,0,0)
  --bench N               render N frames and print mean GPU ms per pass
  --focal PX              override the capture focal length, in scene pixels
  --ipd MM                eye separation in stereo (default 63)
  --smoke N               render N frames in the window, then exit 0
  --scenes DIR            play a folder of scene folders (scene.psplat in each)
  --scale S               initial world scale (default 1, clamped 0.25..4)
  --comfort-radius M      comfort fade radius in metres (default 0.25)
  --fade-start F          radial fade start, fraction of the radius (default 0.5)
  --playlist N            headless: play the playlist N loops, one frame per scene
```

Interactive controls mirror the DualSense/PS5 mapping: `[` / `]` = L1/R1
previous/next scene, the scroll wheel or `+`/`-` = world scale, `R` = recentre
(yaw only), `P` = pause, mouse look and WASD move the head, ESC quits. Moving
beyond the comfort radius no longer hard-clamps the camera; the image fades to
the backdrop instead, and so does the part of the frame that looks outside the
original photo. The HUD line prints the scene index, world scale and comfort
fade.

## Scene playlist and preload

`core/render/scene_playlist.{h,cpp}` provides `ScenePlaylist` and
`ScenePreloader`:

- A playlist is the immediate subdirectories of `--scenes DIR` that contain
  `scene.psplat`, sorted by folder name; a folder given directly is a
  one-entry playlist. `next()`/`previous()` wrap around.
- `ScenePreloader` runs `core/scene`'s `read_scene_path()` on a background
  thread. The main thread calls `SplatRenderer::stage_scene()` to upload the
  result into a **second** set of five fp16 attribute buffers; the active
  scene keeps rendering from the first set. If the staged scene needs more
  instances than the current capacity, it also stages its own
  instance/key/value buffers and radix sorter.
- `commit_staged_scene()` waits for the device to idle, swaps the buffers,
  rewrites descriptor bindings 1–8 and only then frees the old set. Nothing is
  half-uploaded, so no frame shows a partial scene; the old buffers stay valid
  until the swap. The desktop prefetches the next scene and stages it as soon
  as it lands, so pressing `]` is usually just the commit (≈0.1 ms).

Measured on the same host (`$SPLAT_DATA_DIR/runs/render/comfort/scenes`, two
playlist folders: 1,179,648 and 200,000 Gaussians, 1280×873 one frame each,
`--playlist 2`):

| Step | 1.18 M scene | 200 k scene |
| --- | --- | --- |
| background read/validate (`ScenePreloader`) | 80.6 ms | 13.2 ms |
| `stage_scene` (fp16 conversion + GPU upload) | 15.0 ms | 2.6 ms |
| `commit_staged_scene` (device idle + swap) | 0.09 ms | 0.10 ms |
| start of preload to first rendered frame | 110.8 ms | 19–26 ms |
| staged GPU memory | **31.5 MiB** | 5.3 MiB |

The preload budget is +64 MiB at 1.2 M splats; 1,179,648 splats are
33,030,144 bytes of attributes (31.5 MiB), so the measured footprint is about
half the budget. The instance/key/value arrays are shared while the next scene
is no larger than the current capacity. The background thread also holds one
fp32 `SplatScene` (66 MB at 1.2 M splats) until the main thread uploads it;
that transient copy is released at the commit.

F4 also fixed `run_headless` restarting the scene-1 preload at the end of step
0: it discarded the preload started before the loop and reread the same file.
A `--playlist 2` run now reports the first preload (29–89 ms warm) instead of a
second identical read.

Load to first frame for a `.psplat` (1,179,648 Gaussians, 1280×873, warm
filesystem cache) measured 234–304 ms (scene read 85–99 ms), inside the
spec's 1 s budget. A cold first `.ply` load (3.9 GiB of golden tensors in the
same folder competing for page cache) took 595 ms; repeated loads were
≈113–190 ms.

## Measured results

Host: Apple M4, 16 GB, macOS, MoltenVK 1.3.357, Release build. Outputs are
under `$SPLAT_DATA_DIR/runs/render/` (outside the repository, `AGENTS.md`), and
the scene is `$SPLAT_DATA_DIR/golden/photo01_living_room/reference.ply`,
1,179,648 Gaussians.
Pose: capture pose (origin, yaw = pitch = 0), intrinsics from the `.ply`
(fx = fy = 1074.29 px, cx = 640, cy = 436.5).

Screenshots (`fixed/` = `$SPLAT_DATA_DIR/runs/render/fixed/`, 1280×873, one
frame each, about 1.10 M visible Gaussians). Timings are `--bench 20` means at
the same pose and size: a single `--screenshot` frame measures about twice the
draw time, because the GPU is still at its idle clock for that one frame.

| File | Pose | project | sort | draw | total |
| --- | --- | --- | --- | --- | --- |
| `fixed/photo01_capture.png` | 0, 0, 0 | 0.943 ms | 1.394 ms | 8.904 ms | 11.24 ms |
| `fixed/photo01_x-0.05.png` | −5 cm x | 0.934 ms | 1.407 ms | 8.499 ms | 10.84 ms |
| `fixed/photo01_x+0.05.png` | +5 cm x | 0.932 ms | 1.381 ms | 8.609 ms | 10.92 ms |

The 5 cm poses move the whole frame by 13.6 dB worth of parallax against the
capture render, which is what the screenshot diff measures; the timings barely
move.

After F4, the same mono `--bench 20` measures project 0.925 ms, sort 1.476 ms,
draw 8.527 ms, total 10.929 ms. The deterministic full-array sort costs about
0.09 ms (1.18 M entries instead of the ~1.1 M visible); the per-pixel
capture-frustum maths and the D32 depth attachment are within run-to-run noise
at this size.

### Comfort and world-scale screenshots

`comfort2/` = `$SPLAT_DATA_DIR/runs/render/comfort2/` is the F4 set,
`comfort3/` = `.../comfort3/` the stereo-correct F5 set and `comfort4/` =
`.../comfort4/` the F7 set with the 2 % inner ramp, rendered with the same
commands and poses: `photo01_living_room/reference.ply`, 1280x873, radius
0.25 m, head moved sideways along +x (yaw = pitch = 0), one frame each. Fade
values are the `ViewerState` output; the pre-F4 set is still in `comfort/`.

| File (`comfort3/`) | Head x | fade | mean / notes |
| --- | --- | --- | --- |
| `comfort3/photo01_comfort_000.png` | 0 | 1.000 | 162.22 |
| `comfort3/photo01_comfort_030.png` | 0.075 m (30 %) | 1.000 | 161.48 |
| `comfort3/photo01_comfort_060.png` | 0.15 m (60 %) | 0.896 | 157.46 |
| `comfort3/photo01_comfort_090.png` | 0.225 m (90 %) | 0.104 | 124.22 |
| `comfort3/photo01_comfort_060_stereo.png` | 0.15 m (60 %) | 0.896 | 1280x873 per eye, side by side |
| `comfort3/immichvr1_comfort_060_stereo.png` | 0.15 m (60 %) | 0.896 | 1920x1440 per eye, `golden/immichvr/1.ply` |

The capture pose (`000`) is byte-identical to `comfort2/photo01_comfort_000.png`;
30/60 differ from comfort2 only in a band along the right edge (2.4 % / 2.3 %
of pixels), which is the wider soft band. The right edge of `060` and `090` is
the capture-frustum fade: the region the photo never saw is blended into the
backdrop with a 40 px gradient instead of the stretched splat band that
`comfort/photo01_comfort_060.png` still shows.

| File (`comfort4/`) | Head x | fade | mean / notes |
| --- | --- | --- | --- |
| `comfort4/photo01_comfort_000.png` | 0 | 1.000 | 161.81, interior byte-identical to `comfort3/`; 2 % border ring faded |
| `comfort4/photo01_comfort_030.png` | 0.075 m (30 %) | 1.000 | 160.80, ramp starts at x = 1213 (was 1238.6) |
| `comfort4/photo01_comfort_060.png` | 0.15 m (60 %) | 0.896 | 156.85, full opacity ends at x = 1166.4 (was 1192) |
| `comfort4/photo01_comfort_090.png` | 0.225 m (90 %) | 0.104 | 124.12, ramp starts at x = 1123 (was 1148.6) |
| `comfort4/photo01_comfort_060_stereo.png` | 0.15 m (60 %) | 0.896 | ramp starts x = 1185 / 1148, 37 px apart |
| `comfort4/immichvr1_comfort_060_stereo.png` | 0.15 m (60 %) | 0.896 | ramp starts x = 1848 / 1831, 1920x1440 per eye |

The comparison against `comfort3/` is exact rather than statistical: both sets
are deterministic renders of the same scene at the same pose, so the columns
are byte-identical up to the point where the new ramp starts. In the mono 60 %
frame the first differing column is x = 1166 (per-row range 1163–1171), i.e.
the old full-opacity end 1192 minus 25.6 px = 2 % of 1280; the fade end is
unchanged (1224 → 1222, the outer end did not move). The strip from 1167 to
1191 — the 20–25 px of weak border Gaussians the visual check flagged — drops
from a mean red of 190.3 to 180.8 (backdrop 118), and its mean horizontal
gradient from 12.97 to 11.61: the speckle is partly blended away instead of
ending at full brightness. The same 25.6 px shift appears at 30 % and 90 %
(1213 and 1123) because the fade is measured in capture-image units, and 38.4 px
at 1920 wide in `immichvr1`. In stereo the ramp start keeps the per-eye
disparity (37 px measured against 38 px of scene disparity at the wall), so the
wider band did not become screen-locked.

In the stereo pair the fade edge sits on the wall, not on the screen: the fully
faded x is 1241.5 (left eye) and 1202.5 (right eye), 39.0 ± 1.1 px apart, while
the wall texture 100–140 px inside the fade has 38 px of disparity (37 px
180 px inside). The left eye's leftmost pixels are textured like the rest of
the eye instead of the smooth smear the pre-F5 stereo frame had. The immichvr
pair shows the same behaviour over a much larger depth range: the near ground
at the lower right fades, the far trees and building (tens of metres) stay
inside the capture image, as they should — a distant surface barely moves
relative to the eyes. `comfort2/photo01_comfort_120.png`
remains exactly the neutral backdrop (0.18 linear = 118/255 encoded) with the
splats still submitted.

The world-scale screenshots in `comfort/` predate this pass. Scale is about the
capture point, so they are unchanged by the fade rework:

| File | Head x | scale | image mean (0-255) |
| --- | --- | --- | --- |
| `comfort/photo01_scale_0.5.png` | 0 | 0.5 | 162.22 |
| `comfort/photo01_scale_2.0.png` | 0 | 2.0 | 162.22 |
| `comfort/photo01_scale_0.5_x+0.10.png` | 0.10 m | 0.5 | 160.26 |
| `comfort/photo01_scale_2.0_x+0.10.png` | 0.10 m | 2.0 | 161.71 |

At the capture pose the 0.5x and 2.0x renders are byte-identical to the 1.0x
render, as they must be: scale is about the capture point, and the projection
is exactly scale-invariant there. (Before the determinism fix the same
comparison measured 46.1 / 46.3 dB; the difference was tie order, not scale.)
The `x+0.10` pair shows the scale difference through parallax (12.5 dB between
them).

PSNR against the input photo
(`$SPLAT_DATA_DIR/golden/inputs/photo01_living_room.jpg`, 1280×873, no resize
needed):

| Compare | Before the F1 fix | After |
| --- | --- | --- |
| capture pose vs photo | 20.34 dB | **23.21 dB** (MSE 310) |
| −5 cm x pose vs photo | — | 12.80 dB |
| +5 cm x pose vs photo | — | 12.77 dB |
| vs the independent numpy renderer, `eps2d` 0 | — | **36.93 dB** |
| vs the visual-check reference (`ref.py`, `eps2d` 0.3) | 30.00 dB | 26.82 dB |

The row against the independent numpy renderer is the renderer-to-renderer
number the spec's end-to-end test asks for (≥ 35 dB): that renderer shares no
code with ours, implements the same EWA maths and the same linear-light blend,
and uses SHARP's own `eps2d`. 36.93 dB means the two agree to the level of
8-bit quantisation, which is what retires the old claim that the poor
capture-pose PSNR was the scene's fault.

This whole table is a **before-AA** measurement: it was taken with eps2d 0, the
renderer's default until the anti-aliasing change below. With the shipped
eps2d 0.3 the capture-pose render of `immichvr-1` is 28.13 dB against its source
photo where eps2d 0 gives 29.38 dB (and the blunter eps2d 1.0 gives 26.23 dB);
the comparison against the numpy renderer, which renders at eps2d 0, moves the
same way. The filter is a display prefilter, so it is a deliberate trade against
SHARP's still convention — see "Anti-aliasing (AA)".

The last row moves the other way on purpose. The visual check's `ref.py` renders
with `eps2d` 0.3, which SHARP does not use, and the pre-fix build also used 0.3,
so those two happened to match each other (30.00 dB) while both were blurred
relative to the reference. With SHARP's setting the render is sharper and the
agreement with that particular PNG drops to 26.82 dB; against a numpy reference
at `eps2d` 0 it is the 36.93 dB above.

The PSNR against the photo itself is dominated by the reconstruction error of
the reference `.ply`, not by the renderer: the independent numpy renderer only
reaches 20.78 dB on the same scene, and the same executable reaches 36.40 dB
against `golden/immichvr/2.jpg` (2158×1440, `golden/immichvr/1.jpg` at
1920×1440 gives 30.16 dB; the pre-fix build gave 34.11 dB and 28.27 dB). The
5 cm poses score ~12.8 dB by construction: the photo was taken from the capture
pose, so a lateral shift moves the whole frame (13.6 dB against the capture
render) and the comparison measures parallax.

Re-encoding the same scene to `.psplat` (fp16) and rendering the capture pose
again gives 46.30 dB against the `.ply` render, so the fp16 upload path and the
`.psplat`/`meta.json` loader are faithful (the rest is fp16 quantisation plus
the unstable order of depth ties).

`--bench 300 --size 1920x1080 --stereo` (1920×1080 per eye, 3840×1080 total,
1,179,648 splats, mean of 299 frames after one warm-up):

| Pass | Mean GPU ms |
| --- | --- |
| project (cull + project + indirect command) | 0.960 |
| sort (radix, indirect count) | 1.389 |
| draw (both eyes) | 20.654 |
| **total** | **23.003** |

Draw grew by about 1.1 ms against the pre-fix build because the corrected
covariance and the wider cull reach keep the quads the reference actually
draws.

Project + sort is 2.35 ms for 1.18 M splats, inside the spec's 4 ms budget
(2.5 ms stretch). Draw is over the 16.6 ms frame budget at this resolution on
this host. Most of it is per-instance/quad setup for 2.2 M instances (two
eyes), not fill: a diagnostic build that replaced every splat with a 1-pixel
quad still measured 17.4 ms, and the real instance gather adds about 2 ms on
top. MoltenVK on an M4 is a convenience build, not a parity target; the
PS5/RADV numbers are the ones to compare with the budgets.

## Determinism

The pre-fix pipeline appended visible splats with `atomicAdd`, so the slot order
(and with it the order of equal depth keys after the radix sort, which is not
stable across partitions) changed between frames: three runs of the same capture
pose differed in about 0.26 % of pixels by more than 10 levels. The fix keeps
the atomic counter for the draw count only and writes keys, values and instances
at `slot == splat id`; the sort then orders the whole array (culled splats carry
the largest key). The vendored sorter is deterministic for identical input
(checked in `tests/render_sort_test.cpp`), so the frame is a pure function of
the scene and pose:

- three `--screenshot` runs of the same pose now produce **byte-identical**
  PNGs (maximum pixel difference 0, against 0.26 % of pixels over 10 levels
  before);
- `tests/render_determinism_test.cpp` renders a scene of 512 coincident,
  equal-depth half-opacity splats (where tie order changes the blend) four times,
  and a second renderer over the same scene once, and requires every frame to
  match byte for byte.

## Self-review (F4)

Checked while making these changes:

- **Preload thread and Vulkan.** The `ScenePreloader` worker only calls
  `read_scene_path()` (`core/scene`, pure CPU); it never touches a queue,
  command pool, descriptor or buffer. Uploads (`stage_scene`) and commits run on
  the main thread. Queue and command-pool use are single-threaded: one frame is
  recorded, submitted and waited on before the next reset.
- **Buffer lifetimes and the swap.** `commit_staged_scene()` and
  `discard_staged_scene()` call `vkDeviceWaitIdle()` before destroying the old
  attribute/instance buffers, so no buffer is freed while the GPU may still read
  it and the swap is fully fenced. `stage_scene()` only creates buffers while
  the previous frame has completed.
- **Recentre** is yaw only: pitch survives, and the test checks forward and
  strafe after a 0.7 rad recentre.
- **World scale** multiplies splat positions and Gaussian sizes about the
  capture point (`local * scale`, `sigma * scale` in `project.comp`), and the
  half-IPD passed to the eye shift is physical and not scaled, so the stereo
  baseline stays 63 mm at every scale. Camera translation stays in metres, which
  is why scale is visible only through parallax.
- **Capture-frustum fade.** The first version projected only the ray
  *direction* through the capture camera. Self-review on the 60 % screenshot
  showed the smeared band unchanged: with pure sideways translation the ray
  directions are identical to the capture camera's, so a direction-only test
  never fires. The shipped version reconstructs the pixel's *hit point* from
  the nearest splat depth (a D32 attachment written by the splat pass) and
  tests that, which does remove the band (see the screenshots).
- **Depth attachment.** It is created and owned by `SplatRenderer`
  (`create_vulkan_depth_image`), cleared every frame, filled with depth test
  ALWAYS + writes (far-to-near order makes the last write the nearest) and
  sampled by the comfort pass at binding 11. It does not affect the colour: the
  colour blend stays `ONE` / `ONE_MINUS_SRC_ALPHA`, so frames are unchanged
  except for the new fade, and the byte-identical determinism check still
  passes.
- **Projection regression.** `project.comp`'s covariance is still
  `R S Sᵀ Rᵀ` with the full EWA Jacobian, and `render_projection_test` pins the
  eps2d 0 conic (`low_pass_eps = 0` there); the deterministic slot change only
  touches the key/value/instance writes. The
  numeric-Jacobian, single-Gaussian conic and projection tests pass.
- **Preloader state.** `wait()`/`cancel()` now always return to idle and clear
  the loaded scene and error (the old code only reset `running`, so a failure
  stayed latched and the viewer logged it every frame). `path()` returns a copy
  under the mutex. The viewer remembers a failed path instead of retrying it
  every frame, and `run_headless` no longer restarts the preload it already
  started before the loop.

## Tests

`tests/render_sort_test.cpp` sorts random 32-bit keys with ties (keys in
[0, 1024) and every 8th key full range) at 1, 1000 and 1.2 M elements and
checks the sorted key sequence against `std::stable_sort` exactly, plus that
the value permutation and every (key, value) pair survive the sort. Ties are
not stable across GPU partitions, so only the key order is compared, not the
order within a tie.

`tests/render_projection_test.cpp` renders one Gaussian at
`(0.1, −0.05, 1.0)` m with fx = fy = 100 px and cx/cy = (64, 48) at 128×96 and
checks the intensity centroid is `(74, 43)` px (±1 %), i.e.
`cx + fx·X/Z`, `cy + fy·Y/Z`. A second case renders one off-axis, rotated,
anisotropic Gaussian (4:2:1 scales, 30° about z and 40° about x) and compares
the **second moments of the rendered footprint** against the conic computed by
`core/render/project_math.h` (±2 %). A transposed rotation flips the xy moment
and moves xx by 14 %; a transposed Jacobian moves xx by 11 % and xy by 23 %, so
both regression bugs fail this test.

`tests/render_projection_math_test.cpp` is CPU-only. It pins
`quaternion_to_matrix` (90° about x gives `R`, not `Rᵀ`), the review's
hand-derived depth-only case (`P = (1, 2, 2)`, f = 100 → `[[625, 1250], [1250,
2500]]`), and the full off-axis conic against a **numerically differenced**
Jacobian, plus the eigen-decomposition of the conic and the low-pass term.

`tests/render_flicker_test.cpp` is the AA reproduction and regression guard
(four cases): (1) CPU-only, projects the external `immichvr-1` scene through
`project_math.h` at the PSVR2 eye and at the capture intrinsics and requires the
headset's median splat σ to be below the capture's and more than 40 % of its
visible splats below 1 px (measured 0.55 / 1.23 px and 84 % / 26 %); (2) the
flicker bench above, which also writes the PNG crops and flicker maps to
`$SPLAT_DATA_DIR/runs/aa/`, requires the shipped eps2d 0.3 to cut edge J to
under 60 % of eps2d 0, to beat the uncompensated variant's still fidelity and
the UNORM target's flicker, and to keep project/draw time within noise; (3) the
capture-pose still against `original.jpg`, pinning the ≤1.5 dB price and the
worse result for eps2d 1.0; (4) the near-fade ramp, one splat at 4.9–10 cm,
where the hard cull steps by the whole splat between two frames and the fade
follows smoothstep(0.05, 0.08, z). All four skip when the external scene is
absent; the four cases run in about 3-4 s together (well inside the suite's budget).

`tests/render_preload_test.cpp` stages a 3-splat scene over a 1-splat scene,
renders between stage and commit to prove the old scene and its buffers are
still active, then commits, checks the generation and visible counts, grows the
capacity with an 8-splat staged scene, and confirms `discard_staged_scene()`
leaves the committed scene untouched. It creates a headless Vulkan context and
is skipped when no conforming device exists.

`tests/render_comfort_test.cpp` renders a full-viewport splat with a coloured
backdrop, checks that `comfort_fade = 0` turns every sampled pixel into exactly
the encoded backdrop (the final blend covers the splat image), and that
`comfort_fade = 1` leaves the white splat visible. A second case sets a capture
image half the width of the render with a 25 % outer margin and checks that
pixels whose rays land inside it keep the splat, pixels past the margin are
exactly the backdrop, the mid-margin pixel is a blend and the transition is
monotone: the capture-frustum fade has no hard line. (The default 2 % inner
margin is sub-pixel at that capture size, so the border itself is still at full
opacity there.) A third case pins the inner ramp: the same capture image (32 px
wide in a 64 px frame) rendered twice, with `capture_inner_margin` 0 (the old
ramp from the border) and 0.25 (8 px), where both margins are 25 % so the ramp
spans x = 24 … 40. It checks that the frame is byte-identical left of the ramp,
fades on all four borders, never brightens a pixel, and that the fully opaque
limit moves to x = 24, the ramp start: the measured first differing column is
25, while the old ramp only starts at the border (x = 32). It also asserts the
0.02 default. The last case is the stereo guard: it renders both eyes of a
fronto-parallel white plane with a black/white step at 60 % of the comfort
radius (head x = 0.15 m, 320x240 per eye, capture width 192 px, z = 1 m) and
measures the 50 % fade crossing and the step in each eye. The step's stereo
disparity (the scene disparity near the fade) is 12.61 px and the fade edge's
is 12.58 px, within the 2 px bound; the old midpoint version put both fade edges
at the same screen x (0 px disparity) and fails this test.

`tests/render_determinism_test.cpp` is the frame-determinism guard: 512
coincident splats at identical depth with half opacity (tie order changes the
blend), rendered four times, plus a second renderer over the same scene. Every
frame must be byte-identical.

`tests/comfort_viewer_state_test.cpp` is CPU-only: fade at 0/50/60/75/90/100/120 %
of the radius, the 0.5 default fade start, monotonicity, distance-only (frame
rate independent) output, adjustable radius and fade start, recentre
position/yaw/pitch math (forward/strafe after a 0.7 rad recentre), world-scale
clamping and its independence from the camera pose.

`tests/comfort_playlist_test.cpp` builds temporary scene folders and checks
sorted discovery, wrap-around next/previous, direct-folder and empty-folder
handling, and that a background preload leaves the previously loaded scene
valid until `take()` swaps it.

`tests/vr_view_test.cpp` is the VR2-fix reproduction. It drives `HmdView` -
the same pose mapping, placement and comfort state the title's `vr_mode.cpp`
uses - with synthetic Hmd2-convention poses and renders the real
`runs/sharp_e2e/immichvr-1` scene at 1984×2048 per eye through this renderer.
It checks that the placement frame equals the desktop capture pose in both
eyes (PSNR ≥ 20 dB overall and over the photo region, at least 95 % of the
warm-up frame's visible splats), that a +30° head yaw moves the content left
and a 10 cm head rise moves it down (cross-correlation), and that draw time is
in the normal range. It also checks the renderer's near cull and screen-radius
cap with one splat at 6 cm and one at 3 cm. PNGs go to
`$SPLAT_DATA_DIR/runs/vr2_fix/`.

The Vulkan cases create a headless context and are skipped without Vulkan.

## Files

```
core/render/vulkan_context.{h,cpp}   instance/device, buffers, colour/depth images
core/render/radix_sort.{h,cpp}       wrapper around the vendored sorter
core/render/vk_radix_sort.h          vendored vulkan_radix_sort 0.6.0 (MIT)
core/render/project_math.h           CPU reference for the EWA projection
core/render/camera.h                 CameraPose / FrameView (no Vulkan types)
core/render/viewer_state.{h,cpp}     anchor, recentre, world scale, comfort fade
core/render/hmd_view.{h,cpp}         Hmd2 tracker pose -> scene camera, placement
core/render/scene_playlist.{h,cpp}   scene folder playlist + background preloader
core/render/splat_renderer.{h,cpp}   C6 renderer + one-deep scene staging slot
core/render/shaders/project.comp     cull + EWA project + keys/instances
core/render/shaders/finalize.comp    indirect draw command
core/render/shaders/splat.vert       instanced quad, per-eye centre
core/render/shaders/splat.frag       Gaussian alpha, premultiplied blend
core/render/shaders/comfort.vert     full-target triangle
core/render/shaders/comfort.frag     final comfort/backdrop blend
core/scene/scene_folder.{h,cpp}      scene folder + read_scene_path() loader
platform/desktop/main.cpp            splat_desktop viewer and headless modes
tests/render_sort_test.cpp           radix sort parity
tests/render_projection_test.cpp     single-Gaussian pixel + conic-vs-CPU test
tests/render_projection_math_test.cpp  EWA maths, numeric-Jacobian check
tests/render_preload_test.cpp        staged-scene swap validity
tests/render_comfort_test.cpp        final blend and capture-frustum fade
tests/render_determinism_test.cpp    same pose renders byte-identical frames
tests/comfort_viewer_state_test.cpp  fade, recentre, scale clamping
tests/comfort_playlist_test.cpp      playlist wrap + preload lifetime
tests/vr_view_test.cpp               VR placement/yaw/rise + near/radius cull
tests/render_flicker_test.cpp        headset-eye flicker + AA + near fade
```

Shaders are compiled from `core/render/shaders/` into the build's `shaders/`
directory by the `splat_shaders` target; the renderer loads them from
`SPLAT_SHADER_DIR`.

## Attribution

The projection/EWA math and the quad shaders are ported from
[vkgs](https://github.com/jaesung-cs/vkgs) (MIT, Copyright (c) 2024
jaesung-cs). The radix sort is the vendored
[vulkan_radix_sort](https://github.com/jaesung-cs/vulkan_radix_sort) 0.6.0
(MIT, same author). See `THIRD_PARTY_NOTICES.md`.

## Limitations / not in this task

- Buffers are host-visible; a device-local staging path for a discrete GPU
  (and the PS5) is not implemented.
- One command buffer, submitted and waited on each frame; the viewer does not
  overlap CPU and GPU frames.
- No foveated rendering, no eye-tracked resolution, no reprojection. Those
  belong to C7.
- Per-eye covariance is approximated by the midpoint conic; only the centre is
  reprojected per eye. At 63 mm IPD this is invisible at scene distances but
  is not exact for very close splats.
- World scale multiplies splat positions and Gaussian sizes about the capture
  point; camera translation stays in physical metres.
- A single fallback FOV (60° horizontal) is used when a `.ply` has no
  `intrinsic` element and no `meta.json` is available.
