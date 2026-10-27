# VR scene scale and anti-aliasing research

Date 2026-10-27. Read-only research; no code changed. Sources inline.

Conventions used (core/render/viewer_state.h, hmd_view.h): a scene point x is displayed at
`w = s*x + o` in the placement frame (s = world_scale, o = scene_offset); the camera/eyes are at
the head `h` (`placement_head()`), the viewer never moves on a scale press.
`ViewerState::scale_about_pivot(f, p)` does `s' = f*s`, `o' = p + f*(o - p)` (viewer_state.h:56-57 comment;
runs/vr7/REPORT.md s2). VR7 pivot = h + 1.5 m along `view_yaw()`, clamped to `s*near_depth`
(hmd_view.cpp:251-274).

## Q1 - Scaling that reads as "the scene got bigger/smaller"

### Geometry
Scale `w' = p + f(w - p)`. Write it as scale about the eye plus a translation of the scene:
`w' = h + f(w - h)  +  (1 - f)(p - h)`.
- Term A, scale about the eye (VR6, p = h): every ray through the eye is unchanged, so each
  eye's monocular image is IDENTICAL. Only binocular disparity changes: a point at depth z has
  disparity ~ ipd*F/z pixels (F ~ 830 px for a ~100 deg, 1984 px eye, my estimate). With
  ipd 63 mm: 26 px at 2 m, 5 px at 10 m, 2.6 px at 20 m; at f = 4 these become 6.5, 1.3, 0.65 px.
  Head-motion parallax scales the same way (1/z). A one-photo shell lives at 1-20 m and a
  seated user barely moves, so term A is nearly invisible. That is exactly "scale does absolutely
  nothing". Note scaling the world by f about the eye is equivalent to scaling eye separation by
  1/f with the scene fixed (the hyperstereo / miniature effect: larger modelled eye base makes things
  look nearer and smaller, Renner 2015, van der Hoort 2011, summarised in
  https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7182220/ and Kim & Interrante 2017
  "Dwarf or Giant" https://unpaywall.org/10.2312%2Fegve.20171353 - only large IPD changes, ~150 mm,
  gave a clear size effect; small ones did not).
- Term B, translation `(1-f)(p-h)`: a dolly along the line eye->pivot of `(1-f)*D` metres
  (D = |p-h|). This is the angular change the eye sees, and it is indistinguishable from the viewer
  moving. Optic-flow expansion with no vestibular cue is read as self-motion (vection), which is
  the VR7 complaint "it only moves me forwards or backwards". Unavoidable for any pivot != eye:
  there is no pivot that gives a large angular change AND reads as "world changed", except
  that the pivot's own image point stays fixed, which is the strongest "the thing grew around that spot" cue.

Per-point result for pivot at depth D, point at depth z (same ray as pivot): new depth D+f(z-D).
Content in front of the pivot comes toward the eye, content behind recedes; lateral offsets from the
pivot ray scale by f. Far content (z >> D) barely changes angularly. So:
- Pivot = nearest surface ("scene front", D = d_near): the foreground stays put in the picture
  and in depth; everything behind expands/deepens away from it; nothing can come through the eye for
  f >= 1, and for f < 1 the whole scene shrinks toward the front surface (a miniature that sits
  in front of you). Reads as "the room got deeper/bigger behind this". Safest comfort-wise.
- Pivot = scene centre (D = d_mid, median depth): the middle stays, front surfaces rush toward you
  (f>1) and can reach the eye when D + f(d_near - D) < ~0.3 m; far
  surfaces retreat. Reads the most like "the whole scene object scaled" (dollhouse / diorama
  behaviour), strongest for f<1 (miniature in front of you). Needs the near-clamp below for f>1.

### What other apps do (and what I could and could not verify)
- Google Earth VR: two-controller grab; world scaled/translated/rotated about the grip midpoint;
  comfort mode fades a peripheral tunnel + stable grid during movement
  (https://blog.google/products/google-ar-vr/daydream-labs-locomotion-vr/). I found no source with
  exact tunnel parameters.
- Unreal VR Editor world scaling: scale centre is the midpoint of the two hands, or optionally the
  tracking-space floor, with the centre recomputed per gesture and a setting to allow simultaneous scale +
  rotate (https://dev.epicgames.com/documentation/unreal-engine/API/Editor/ViewportInteraction/UVISettings).
  Lesson: pivot is a gesture-start decision, frozen for the gesture (VR7 already does this).
- Needle Engine / Unity "squeeze to scale world": scales the player XR rig (inverse factor) rather than the
  content (https://engine.needle.tools/docs/community/contributions/web3kev/squeeze-to-scale-object-or-world-in-vr).
  Scaling the rig by 1/f about the pivot is the same transform as scaling content by f, but rig scaling also
  scales IPD, head-motion gain and near plane; with content scaling you must keep IPD and head gain physical.
- Gracia, Scaniverse/Polycam VR viewers, SteamVR world-scale: I found no documentation; do not rely on them.
  (SteamVR's "world scale" is rig scaling, same as above.)
- Comfort: Meta vignette guidance (https://developers.meta.com/horizon/design/locomotion-comfort-usability/).

### Recommendation
1. Offer both pivots as a toggle, default "scene front". Per press freeze the pivot (as VR7).
   Show the active one in the HUD.
2. Do NOT also scale eye separation. Scaling IPD by 1/f cancels the only cue that term A produces,
   and large IPD changes cause discomfort. Keep physical IPD (hmd_view.cpp:375). Only for the
   explicit "miniature/dollhouse" feel, a stereo boost (ipd*k, k<=1.5) is an optional later knob, not now.
3. Pivot from splats (CPU, once per press, strided sample of every 8th-16th splat with opacity > ~0.3, in
   displayed space `w = s*x + o`, view-space depth along the horizontal gaze `g`):
   - Cone: keep splats whose direction to the eye is within ~15 deg of the head forward (use full head
     forward, not just yaw, so looking down at a table works). If fewer than ~200 samples, widen to 30 deg,
     then to the whole scene.
   - front: `d_front = clamp(percentile_5(depth), 0.3, 30)`; centre: `d_mid = clamp(percentile_50(depth), d_front, 60)`.
   - Pivot point: `p = h + d * u`, with u = unit vector of the cone's median direction (or head forward);
     use the actual 3D hit position (mean of splats within +-10 % of the percentile depth) so the pivot
     lies on the shell. The gaze-ray hit (eye tracking) is better but PSVR2 gaze is not required; head forward is enough.
   - Percentile, not min: a 1st percentile (VR7's `near_scene_depth`) over the whole scene is dominated by
     floaters/edge splats and ignores where the user looks; the cone fixes that.
   - Centre-pivot guard for f > 1: the nearest content moves to `D + f(d_near - D)`; clamp the allowed f so that
     value stays >= 0.5 m (`f_max = (D - 0.5)/(D - d_near)` when D > d_near). The renderer's 8 cm near fade
     is a last resort, not a plan.
4. Formula for the existing transform (placement frame, y down):
   `f_applied = clamp(s*f, 0.25, 4)/s` (already done)
   `s' = f_applied*s`
   `o' = p + f_applied*(o - p)`
   with `p = h + d*u` (h = `placement_head()`). Derivation: the point of the scene displayed at p must
   still be displayed at p: `s' x + o' = p` for `s x + o = p`. Eye-centred (VR6 variant, kept only as a
   debug option): `p = h`. Frozen for the whole press; for a two-hand pinch, `f = |hands_now|/|hands_start|` and
   `p` = hand midpoint at start (Unreal's default).
5. Visual feedback: a small ring/sphere marker at the pivot (fade in on press, out 0.5 s after release),
   a billboard readout "Scale 1.8x - front" next to it (existing `Scale N.Nx` plate can host the pivot name),
   and a faint depth-fixed reticle line from the eye. Also fade a peripheral vignette in proportion to
   |d ln s/dt| (Meta/Google tunnel idea) because the dolly term B is optic flow.
6. Rate limits (comfort): keep VR7's x2 per 3 s at full press (0.23 ln-units/s ~ 23 %/s) and 0.5 s ease-in;
   cap the dolly speed `|(1-f)*D|/dt` <= ~1.5 m/s (lower D or slower f when D is large), ease out 0.3 s,
   no scaling while the head angular speed exceeds ~60 deg/s. For jumps (reset, preset sizes) animate over
   >= 0.6 s, never a hard cut. Range 0.25x-4x is fine.
7. Make term A visible too: since the stereo cue is weak beyond 5 m, consider a short (1-2 s) "reference"
   during the press: the pivot marker plus faint horizontal rings on the ground at 1 m and 2 m scaled WITH the
   scene, so the user sees the scene unit change against a fixed-size marker. Optional; not core.

## Q2 - Anti-aliasing: ImmichVR vs ours

ImmichVR (a comparable Immich VR client, cloned for review; services/frontend). Splats are rendered by Spark (`@sparkjsdev/spark` 0.1.10, package.json:24;
services/frontend/src/components/GaussianSplatViewer.jsx:4,10,141) inside three.js 0.182 +
`@react-three/fiber` + `@react-three/xr` 6.6.29. It only sets `maxStdDev` (3.0 HIGH / 2.0 LOW,
GaussianSplatViewer.jsx:86-96) and `scale`; every other Spark setting is its default. The XR store
(src/components/xr/xrStore.js:5-8) sets no framebufferScaleFactor/foveation, the splat Canvas is
`gl={{antialias:false}}` (VRThumbnailGallery.jsx:1403), `dpr` is irrelevant in XR.

Spark 0.1.10 defaults (node_modules dist/spark.module.js, from `npm pack @sparkjsdev/spark@0.1.10`):
- blurAmount 0.3 px^2 and the Mip-Splatting style `sqrt(detOrig/det)` alpha compensation (lines ~10434,
  vertex shader `blurAdjust`). Identical to our eps2d 0.3 + compensation (project.comp:29-36, 217-224). NOT a difference.
- preBlurAmount 0, minAlpha 0.5/255 (ours: 1/255 in project.comp, 1/512 in splat.frag:24), maxPixelRadius 512,
  falloff 1 (alpha = a*exp(-0.5 z)), fragment discards z > maxStdDev^2 with default maxStdDev = sqrt(8) = 2.83 sigma
  (ImmichVR overrides to 3.0). Ours reach is 3 sigma. Tiny difference, no truncation correction in either.
- premultipliedAlpha true; `encodeLinear` false for a normal sRGB canvas, so Spark blends sRGB-encoded colours
  directly (gamma-space blending). We blend in linear light (splat.frag:16-31). Gamma-space blending makes
  semi-transparent edges look softer/brighter-darker differently; both are valid, ours is "correct".
- sortRadial true (sort by distance to the eye, rotation-invariant, better for HMD), 16-bit double-sort by default,
  CPU worker sort, can lag a few frames. Ours: GPU radix sort (radix_sort.cpp), by view depth. Sorting precision
  is a popping issue, not an edge-smoothness issue.
- maxSh = 3 but SHs come from the .ply/.spz; one-photo SHARP splats are SH degree 0 either way. Not a difference.
- Dithering: none. MSAA: off (and useless for sorted alpha splats). Foveation: unset (browser/runtime default).

### Differences that could explain "slightly smoother", ranked
1. Render resolution and the display chain (HIGH impact, free on desktop, costly on PS5). WebXR on SteamVR/PCVR
   typically uses the runtime-recommended eye buffer (SteamVR auto-scales ~1.4x per axis supersampling on strong
   GPUs) and the compositor resamples to the panel with lens distortion. Our 1984x2048 is 1:1 with the panel pre-distortion,
   so after lens warp it is effectively undersampled at the periphery; any shader aliasing shows. I could not
   confirm ImmichVR's actual eye buffer size (not logged in its code). Cost on PS5: supersampling 1.25x/axis
   = 1.56x fragments; our fill is the bottleneck at 60 fps. A cheaper trial: render 1.2x only when the splat count/frame
   time allows (dynamic resolution), or sharpen nothing.
2. Frame timing: 60 fps reprojected to 120 Hz (ours) vs native 90/120 fps WebXR. Reprojection of a 2.5D shell smears
   edges and can read as "less crisp/steady". Not AA but affects perceived smoothness. Cost: nothing to change besides
   frame budget.
3. Gamma-space (sRGB-direct) blending in Spark vs our linear blending (medium-low impact, free). Gamma-space
   blending of semi-transparent splats yields darker, softer-looking edges for bright-on-dark transitions;
   we could test an `srgb_target=0` path (`camera.params.z`, splat.frag:25) for A/B. Free.
4. Gaussian cutoff 2.83 sigma (Spark default) / 3 (ImmichVR) vs our 3 sigma with alpha < 1/512 discard, and our
   hard 8 cm near fade: negligible. minAlpha 0.5/255 vs our 1/255: negligible (a faint ring at most).
5. Same 0.3 px^2 low-pass and compensation in both, so the kernel is not the cause. If we want smoother
   without resolution cost, try eps2d 0.4-0.5 (cost 0, loses sharpness) - a taste knob, A/B in the headset.
6. Sort order: Spark's radial sort is more view-consistent at the periphery for a wide FOV; our depth sort can
   flip splat order at the edge of a 100+ deg view. Low impact on edge smoothness, free to try radial key
   in project.comp (key = length(view_pos) instead of z).
7. MSAA / dithering / SH: not factors (neither side uses them meaningfully).

Highest-value, lowest-risk experiment: A/B eps2d (0.3 vs 0.5) and a 1.2x eye buffer on PS5 measuring frame time,
plus the linear vs gamma blend toggle. Unverified: ImmichVR's real eye resolution and the PC headset/runtime used.
