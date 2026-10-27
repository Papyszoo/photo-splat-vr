# C4 fallback backend — Gaussian parameterisation and visual quality

How the fallback (Depth Anything V2 Small) turns a photo into the `.psplat`
scene, and what the rendered capture pose and ±5 cm head offsets look like
against the photo. The renderer conventions this has to satisfy are in
`docs/render.md` (SH-DC colour `0.5 + C0·sh_dc` then sRGB decode, opacity
`sigmoid`, log-scale, `eps2d = 0`) and the file layout in
`docs/formats/psplat.md`.

## What one run emits

`FallbackReconstructor::run()` first snaps Depth Anything's smooth silhouette
ramps onto the two surfaces they join (see "Depth ramps" below), then emits two
kinds of Gaussians, in OpenCV axes (x right, y down, z forward):

1. **Surface layers** on a regular grid over the photo, one per full-resolution
   depth layer found in a cell (one normally, two at a silhouette).
2. **Disocclusion fill** behind foreground edges: the far-side depth and colour
   extended under the near side, so a translated head sees background instead
   of the neutral backdrop.

The fill is built from a full-resolution, per-pixel near/far classification of
every depth edge. Colour **and** depth of a fill sample come only from a
far-side pixel; the near side is the destination of the dilation, never a
source. Fill samples sit at the far depth pushed behind by a small margin, and
one sample is emitted per 2x2 grid cells inside the revealed band.

| item | value |
| --- | --- |
| grid | `fallback_grid()` with 95 % of the target (a reserve for the fill band): `columns = round(sqrt(target·w/h))`, `rows = round(target/columns)`, both clamped to the photo size |
| position | covered-area centroid of the layer in continuous pixel coordinates `(px, py)`, unprojected at the layer's metric depth `z`: `x = (px − w/2)·z/f` |
| x/y sigma | `0.72 · z · cell_px / f` metres, i.e. **0.72 cell** in pixels at the capture pose (0.72·√area fraction for a partial layer) |
| z sigma | `0.25 · min(cell_x, cell_y)` metres (a thin depth layer) |
| colour | linear-light area average of the layer's full-resolution pixels, encoded to SH DC |
| fill spacing | one sample per 2x2 grid cells (`kFillBlockCells`) |
| fill x/y sigma | `0.9 · 2 · z · cell_px / f` metres (a little wider than the 2-cell spacing, so the revealed strip has no 1 px seam) |
| fill z | `z_far · 1.002`, i.e. strictly behind the far-side surface it was sampled from, capped at `max_depth_m` |
| opacity | `0.99 · covered fraction of the cell` (0.99 for a normal cell, 0.99 for fill) |
| rotation | identity |

The centroid is the exact covered-area centroid, not the pixel index: for a
fully covered cell it is the cell centre, and for a layer it stays on the
surface it belongs to. This is what keeps the reconstructed image aligned to
the photo (see "Alignment" below).

Counts for the 0.5 M target (grid cells, surface layers including split cells,
fill samples, total):

| photo | size | grid | cell | surface | fill | total |
| --- | --- | --- | --- | --- | --- | --- |
| `immichvr/4.jpg` (pizza) | 1440×1920 | 597×796 | 2.41 px | 475,971 | 9,017 | 484,988 |
| `immichvr/5.jpg` (gondola) | 1440×1920 | 597×796 | 2.41 px | 476,424 | 6,584 | 483,008 |
| `inputs/photo01_living_room.jpg` | 1280×873 | 835×569 | 1.53 px | 475,589 | 2,405 | 477,994 |
| `inputs/photo02_street.jpg` | 1280×720 | 919×517 | 1.39 px | 477,618 | 10,213 | 487,831 |
| `inputs/photo03_reading_room.jpg` | 1280×964 | 794×598 | 1.61 px | 476,264 | 16,434 | 492,698 |

The 5 % grid reserve (475 k cells) is what the fill band is paid from. Once the
ramps are snapped every silhouette is a detected edge, and at one fill sample
per cell the full band needed 33 k (pizza), 35 k (photo02) and 60 k (photo03)
samples against ~24 k of room: the budget cut photo03's band to 25 % of its
design width, i.e. half of what a 5 cm move uncovers, and the rest showed as
backdrop. One sample per 2x2 cells fits the whole band on every photo.

## Why the footprint is 0.72 cell

The old footprint used `0.5 · cell`: at the capture pose a splat's 1σ is half
the distance to its neighbour, so at 0.99 opacity the seam between two cells
only accumulated `1-(1-0.99·e^-0.5)² = 0.84` alpha (0.82 with the old 0.95).
The grey backdrop showed through every cell boundary — the screen-door grid in
`runs/app/job-000001.png`.

With `sigma_px = 0.72 · cell_px` two neighbours contribute
`1-(1-0.99·e^(-0.5·(0.5/0.72)²))² = 0.951` at the worst point of a seam, and
the four diagonal neighbours bring the corner coverage to 97.7 % (98.2 % with
the old 0.75). 0.72 is the smallest value that keeps the pairwise seam test
above 95 %; the old 0.75 was 4 % wider than needed and cost capture sharpness
for no coverage. The renderer draws a 3σ quad, i.e. 2.16 cells, so the tails
always reach the neighbours.

The trade-off is a slightly wider reconstruction kernel than the ideal ~0.5
cell box, i.e. ~0.4 dB of PSNR. Values 0.70 and below start to show the
lattice again in the darkest flat areas. The measured cell-phase modulation on
the five photos is unchanged from the 0.75 pass (pizza 0.0024, gondola 0.0044,
photo02 0.0074, photo03 0.0040; photo01's 0.041 is measured on 16.8 k pixels of
gun/frame edge), so the smaller footprint did not bring the grid back.

## Colour

The renderer draws `rgb = clamp(0.5 + C0 · sh_dc)` and then decodes sRGB before
blending in linear light. The producer therefore has to store the **inverse**
of that mapping — `(encode_srgb(linear) - 0.5) / C0` — which
`fallback_sh_dc_from_srgb_value()` does and
`fallback SH DC is the inverse of the renderer's colour mapping` checks
(0/128/255 round-trip to their 8-bit value, and 128 decodes to 0.2159 linear).

The surface colour is the **area average over the layer** in linear light
(decode each 8-bit sRGB pixel with the exact EOTF, average, re-encode). For a
cell with one depth layer that is the whole cell, weighted by fractional pixel
overlap; for a split cell each layer gets only its own pixels. Averaging in
encoded space instead is ~1 % darker in the mid-tones. Boundary pixels are
weighted by covered area and the centroid uses the covered sub-rectangle, so
there is no sub-pixel position bias.

A fill sample's colour is the mean linear colour of the **far side a little
away from the edge**: the pixels in a patch around its source (±2 network
pixels) that lie on the source's depth layer (within the 0.25 step, so never
the nearer object) and more than one network pixel from any near-side edge
pixel. The edge's own pixels are where the network's edge and the colour edge
disagree by a pixel or two — anti-aliased rims, a near object's rim the depth
put on the far side — and the fill copies its colour across the whole gap, so
reading those pixels gave the salmon/mauve blocks and bands behind photo03's
heads and coats. A far side thinner than the margin falls back to the
far-side edge pixels of the 3×3 patch. The patch is chosen by depth layer and
edge distance, not by the grown near region, because that region leaks into
the floor where a person touches it (it is a flood fill that only stops at
far-side edge pixels).

## Discontinuities, full-resolution split, thin features, fill

**Depth ramps.** Depth Anything's map is smooth at a silhouette: the network
output (short side `input_size` = 518) is upsampled bilinearly, and a depth
step arrives as a ramp 4-16 photo pixels wide whose pixels lie between the two
surfaces (photo03, the bald man's edge: 0.54, 0.60, 0.68, 0.82, 0.94 m from the
man at 0.5 m to the floor at 1 m). The adjacent-pixel steps (0.1-0.2) stay
below the 0.25 edge threshold and a cell's range below the 0.5 split step, so
before this pass the ramp was neither an edge nor split: each ramp cell became
one opaque splat at a depth where nothing is. At +5 cm those splats move 93,
82 and 68 px against 111 px for the man and 59 px for the floor, and the same
depths in neighbouring rows line up into lines parallel to the outline: that
is the black/grey zebra along every person, the beige halo beside the coat, the
brown sawtooth along photo01's rifle stock and the pale rim along the gondola.
SHARP's scene rendered from the same poses with the same renderer shows none
of it, and it persists with the fill disabled (`runs/fallback_rootcause.md`).

`snap_depth_ramps` therefore runs first. A pixel is on a ramp when its parallax
under the 10 cm design head motion differs from a 4-neighbour's by more than
1 px (`kRampParallaxPx`): photo03's floor at 0.5 m stretches 0.5 px per pixel,
a ramp 5-30. Each ramp pixel takes the depth of the nearest off-ramp pixel on
the side it belongs to, searched within 8 network pixels
(`kDepthRampNetworkPx`). The side combines the ramp position `t` (0 at the near
anchor, 1 at the far one, log depth) as the prior with colour as the evidence:
near when `(d_near − d_far)/(d_near + d_far) < 1 − 2t`, where `d` is the squared
sRGB distance to the mean colour of that side's off-ramp pixels in the window.
Depth alone is not enough — on the gondola the network's edge sits ~4 px inside
the water, so a midpoint snap glues water to the boat. A pixel keeps its depth
when one side has no anchor in reach (a rod thinner than the ramp) or when its
two anchors are less than 120° apart: a rod's pixel finds the body it hangs
from along the rod, and snapping only the part within reach of the body split
the gondola's prow rod in two. `fallback snaps the network's smooth silhouette
ramp onto its two surfaces` covers it.

The discontinuity mask is sampled at **full resolution**, not at grid
resolution. Every pixel that touches a relative depth jump above
`drop_relative_step = 0.25` is classified as the **near** or **far** side of
that edge (both sides of a pair are marked). Near-side edge pixels are then
grown into the surface they belong to, stopping at far-side pixels, so a wide
foreground has interior pixels that the band can reach without ever crossing
the silhouette.

Inside each cell the cover is sorted by metric depth; if its total relative
depth range exceeds `drop_relative_step` (0.5) and the largest gap between
consecutive pixels is a real step, the cell splits into a near and a far layer
at that gap:

- Both layers are emitted with their own depth, colour and covered-area
  centroid. A silhouette cell therefore lands within the cell instead of
  being dropped or faded to one mid-depth average, which removes the cell-sized
  scalloping on the arch/sky and the gondola prow (compare
  `runs/render/fallback2/after_venice5_capture.png` and
  `runs/render/fallback4/after_venice5_capture.png`).
- Opacity is the covered fraction: a 60/40 cell emits 0.59/0.40, so the edge is
  anti-aliased at full-resolution accuracy. A partial layer's x/y sigma is
  shrunk by `√fraction`, so a thin foreground sliver stays thin instead of
  becoming a cell-sized blob.

**Thin features.** A near-side region narrower than
`kFallbackThinFeatureCells = 3` cells (the gondola prow rod, posts, hair) is
below the grid's resolution. The far-side edges cut every boundary pixel, so a
plain distance transform would wrongly mark the rim of every large object as
thin. The producer therefore erodes the near region to its thick core
(half-width above 1.5 cells) and dilates the core back by the same radius;
only pixels the dilated core does not reach are thin. A 1–2 cell rod has no
core at all and is wholly thin; a 3-cell-or-wider object keeps its rim.

Thin cells keep the single layer they had before the fill pass, at full
opacity, and are never split or backed with fill. The layer keeps the
whole-cell colour so a 1 px rod does not become a hard dark bar, but its depth
is the near-side median: a whole-cell median can land on the sky side in one
cell and on the rod in the next, and under a 5 cm head move those neighbours
would shift by different amounts and turn the rod into a bead chain. This is
what removes the dark blobs and the white fleck on the prow rod at every pose,
and keeps the capture pose at the pre-fill result there.

**Disocclusion band.** The fill is an iterative dilation of the far-side layer
under the near-side mask. Multi-source BFS starts at every far-side edge pixel;
it copies that pixel's colour and depth into near-side pixels and stops at the
near/far boundary, so a fill pixel only ever holds a far-side value. For a
near pixel at distance `d` from the far side, the band is

```
band_px = f · head_motion · (1/z_near − 1/z_far)     head_motion = 0.10 m
```

(the parallax difference between the two depths; e.g. 75 px for 1 m against
4 m at a 1000 px focal). A pixel participates when `d <= band_px` and it is not
thin. Fill samples are placed one per 2x2 grid cells at the covered-area
centroid of the block's valid fill pixels, with the depth of the pixel nearest
the far edge (colour: see "Colour"). They stand at `z_far · 1.002`, never past
`max_depth_m` (sky at the cap is backed at its own depth): the margin is a few
surface z-sigmas (0.06 % of z at the 500 k grid), so a fill splat can never
z-fight with the surface layer behind it, while the shorter parallax does not
open a seam. (The
old pass used exactly `z_far` and a 1 % margin was tried: at a 100 px band that
slips one pixel and leaves a pure-backdrop line along every silhouette.)

The band is sized for twice the ±5 cm verification poses and is shortened
proportionally (never widened) when the scene budget would overflow; at one
sample per 2x2 cells none of the five photos needs that.
`fallback_disocclusion_band_px` is the formula and is unit-tested.

The **lighter band along the frame border is not a fallback gap** — it is the
C7 capture-frustum fade (`docs/render.md`): `capture_inner_margin = 0.02` starts
the ramp 2 % inside the capture image (28.8 px at 1440 wide) and reaches 33 %
backdrop exactly at the border, deliberately. The backdrop percentages below
are therefore reported both over the whole frame and excluding a 60 px border.

## Alignment

There is no pixel-centre convention error in the producer. The evidence:

- a synthetic single-colour patch at continuous-pixel centre
  `(160.25, 120.75)` on a 320×240 photo reconstructs with its brightness
  centroid at `(160.248, 120.752)` — 0.002 px off. The grid saturates at one
  Gaussian per pixel there, so a half-pixel mistake would show at full size.
  This is `fallback single-colour patch keeps its sub-pixel centre`.
- on the five real photos the render-vs-photo alignment, measured by phase
  correlation after blurring both to the same 3 px kernel (otherwise the
  fallback's wider reconstruction kernel biases the peak toward the sharpest
  content), is **−0.01…+0.01 px in x and −0.04…+0.02 px in y**. The raw
  unblurred correlation used in an earlier review reported ~1 px up/left; that
  was the extra reconstruction blur and the old faded silhouette cells pulling
  the peak, not a translation.

## Measurements

Method: `splat_reconstruct --backend fallback --depth 3 --splats 500000`, then
`splat_desktop <scene.psplat> --screenshot …` (mono, eye size = capture size,
pose `0,0,0,0,0` and `±0.05,0,0,0,0`). Files in
`runs/render/fallback5/`, "before" is the previous pass in
`runs/render/fallback4/`. PSNR is RGB against the source JPEG at full
resolution; "inner" excludes a 60 px border (the capture-fade ring). Backdrop
is the renderer's neutral clear colour (0.18 linear, 118/255 after the sRGB
store); a pixel is pure backdrop when all channels are within 1.5/255 of it.
"dark lum" is the mean luminance of the darkest 5 % of photo pixels; the
columns are **photo / render**.

Capture pose, before (fallback4) → after this pass:

| photo | PSNR | inner | dark lum photo / render (after) | fill / total |
| --- | --- | --- | --- | --- |
| pizza 4.jpg | 28.07 → **28.19** | 29.34 → 29.47 | 0.120 / 0.188 | 9,017 / 484,988 |
| gondola 5.jpg | 24.68 → **24.80** | 25.22 → 25.37 | 0.009 / 0.042 | 6,584 / 483,008 |
| photo01 living room | 18.23 → **18.25** | 18.44 → 18.46 | 0.138 / 0.288 | 2,405 / 477,994 |
| photo02 street | 18.63 → **18.66** | 18.36 → 18.40 | 0.017 / 0.192 | 10,213 / 487,831 |
| photo03 reading room | 20.27 → **20.30** | 19.91 → 19.95 | 0.008 / 0.110 | 16,434 / 492,698 |

Pure-backdrop area at ±5 cm, % of the frame (whole frame / inside a 60 px
border), before → after:

| photo | +5 cm | −5 cm |
| --- | --- | --- |
| pizza 4.jpg | 1.237 / 0.002 → 1.117 / **0.000** | 1.598 / 0.000 → 1.376 / **0.000** |
| gondola 5.jpg | 0.969 / 0.004 → 0.908 / **0.004** | 1.247 / 0.007 → 1.157 / **0.007** |
| photo01 | 1.548 / 0.002 → 1.547 / **0.000** | 0.633 / 0.000 → 0.633 / **0.000** |
| photo02 | 0.850 / 0.067 → 0.820 / **0.054** | 1.308 / 0.049 → 1.246 / **0.052** |
| photo03 | 2.390 / 0.182 → 2.145 / **0.013** | 2.416 / 0.152 → 2.017 / **0.021** |

Interpretation:

- The whole-frame percentages are dominated by the deliberate capture-frustum
  fade ring: at ±5 cm the view leaves the photo by up to ~28 px at 3 m depth and
  the fade turns the outer 40 px into backdrop. The inner numbers are the
  disocclusion metric.
- photo03's zebra and dotted streaks along every person, the beige halo beside
  the coat and photo01's sawtooth along the rifle stock are gone at both poses:
  they were the network's ramp pixels, not the fill (the previous pass' claim
  that the zebra was fixed referred to fill-vs-fill z-fighting; the zebra is
  made of surface splats and was unchanged then).
- The salmon/mauve fill blocks behind photo03's heads and coats are gone; the
  fill now shows the true background colour (floor, red coat, shelves). It is
  still an extrusion of the far edge, so wide gaps read as horizontal streaks
  of that colour, and a small bright object right behind an edge is extruded
  too (a white streak beside the bald man's neck at +5 cm).
- photo03's inner backdrop fell from 0.182/0.152 % to 0.013/0.021 %: the full
  band now fits the budget and the ramps no longer leave holes between their
  lines. The reported 1 px cracks (photo03's right-hand man, pizza's dark
  object, beside photo01's rifle) are gone; ~200 px of short cracks remain in
  photo03, e.g. along the blue jacket's left edge at −5 cm.
- Not fixed: a near object's bright rim that both cues put on the background —
  the gondola's 5 px gunwale highlight is bluer and brighter than the water but
  far closer to it than to the dark boat, and the network's edge is on the
  water side there too — stays on the far plane and shows as a pale-blue line
  beside the gondola at −5 cm (photo03's jacket highlight likewise). The pale
  sky-coloured patch at the prow base is mostly gone; the prow rod is as before.
- Capture PSNR is up on all five photos (+0.02 to +0.12 dB): the ramp cells
  were also slightly wrong at the capture pose. photo01/02/03 remain
  texture-limited at this density: a box downsample to the same grid plus the
  sharpest possible piecewise-constant upsample only reaches 21.4 / 23.2 /
  24.3 dB against its own photo, before any overlap requirement.
- Alignment (dx/dy px, render relative to photo) is unchanged: `−0.00/+0.00`
  pizza, `−0.00/+0.02` venice, `+0.01/−0.01` photo01, `−0.00/+0.01` photo02,
  `−0.01/−0.04` photo03.
- Ghost metric (`FallbackFillStats::ghost_samples`): a fill sample whose
  colour is closer to the near side of the edge it was built from than to the
  far side — the mean colours of the nearer object and of the source's depth
  layer in the patch around the source pixel, rim included — on edges whose
  sides differ by more than `kGhostMinContrast`. The previous version compared
  the fill colour with itself and could never count anything. Now: pizza 31 of
  9,002 (0.3 %), gondola 111 of 6,578 (1.7 %), photo01 0 of 2,408, photo02 404
  of 10,204 (4.0 %), photo03 217 of 16,416 (1.3 %). The ones inspected are
  far-side texture, e.g. water darker than its bright rim, which lifts the far
  mean the fill deliberately skips. A fill that takes the near-side pixel under
  it (injected for the check) scores 83 % on pizza and 64 % on photo02.
- `core/render/` is unchanged; SHARP image parity is the same as before
  (photo01 35.50 dB, immichvr/1 39.10 dB mean over 8 poses).

Screenshots (all in `runs/render/fallback5/`): `after_<photo>_capture.png`,
`after_<photo>_x+0.05.png`, `after_<photo>_x-0.05.png` for pizza4, venice5,
photo01, photo02, photo03. The scenes are in `scenes/<photo>/`; the root-cause
experiments are in `runs/fallback_rootcause.md`.

## Tests

`tests/fallback_scene_test.cpp`:

- `fallback footprint covers the seam between two cells` — pure geometry: two
  neighbours at the capture pose reach 95 % combined alpha at the midpoint with
  the 0.72-cell sigma, and the old 0.5-cell sigma would not (that is the grid).
- `fallback SH DC is the inverse of the renderer's colour mapping` — the shader
  expression round-trips 8-bit colours and 128 decodes to 0.2159 linear.
- `fallback disocclusion band is the head-motion parallax` — 75 px for 1 m vs
  4 m at a 1000 px focal and 10 cm, monotone in the depth difference, zero when
  there is no background.
- `fallback single-colour patch keeps its sub-pixel centre` (data-gated) — the
  synthetic patch above, centroid within 0.25 px (measured 0.002 px).
- `fallback fills the background behind depth edges` (data-gated) — projects
  every splat and counts the samples that sit on the same pixel ray as a
  surface at least 1.25× nearer; reports the exact producer fill count and
  checks it is above 500 and below 1/6 of the scene.
- `fallback fill takes colour and depth only from the far side` — synthetic
  two-plane scene, near red at 1 m and far blue at 4 m. Every splat the fill
  adds is blue and at or behind the far depth; a near-side sample would show as
  red. The producer's ghost count must be exactly 0 here.
- `fallback snaps the network's smooth silhouette ramp onto its two surfaces` —
  the same red/blue planes with the step smeared over a 6 px ramp in inverse
  depth and a sharp colour edge: every splat lies on one of the two planes and
  every red splat on the near one (without the snap: 426 floating, 141 red).
- `fallback leaves a one-cell-wide rod without fill` — synthetic 1-cell rod on
  a far background. With and without fill the scene has exactly the same splat
  count, positions, colours and opacities: no fill and no split on the rod.
- `fallback fill colour comes from the far side of its edge` (data-gated) —
  the producer ghost count stays below 5 % of the fill samples on the
  configured photo (0-4 % on the five photos, 64-83 % with an injected
  near-colour leak).
- `fallback reconstructs a splat scene from a photo` (data-gated) — also
  projects consecutive same-row splats with `render/project_math.h` and checks
  the seam coverage of the real scene: median 0.951, 5th percentile 0.951 on
  `4.jpg`.
- The grid, resize and metric-depth cases are unchanged.

## Reproduce

```sh
export SPLAT_DATA_DIR=${SPLAT_DATA_DIR:-$(pwd)/..}
build-opus-fallback/tools/reconstruct/splat_reconstruct --backend fallback \
  --model "$SPLAT_DATA_DIR/weights/dav2s-f16.gguf" \
  --depth 3 --splats 500000 \
  "$SPLAT_DATA_DIR/golden/immichvr/4.jpg" /tmp/pizza
build-opus-fallback/platform/desktop/splat_desktop /tmp/pizza/scene.psplat \
  --screenshot "$SPLAT_DATA_DIR/runs/render/fallback5/after_pizza4_capture.png"
build-opus-fallback/platform/desktop/splat_desktop /tmp/pizza/scene.psplat \
  --screenshot "$SPLAT_DATA_DIR/runs/render/fallback5/after_pizza4_x+0.05.png" \
  --pose 0.05,0,0,0,0
```

`runs/render/fallback5/run_set.sh` regenerates the whole set and
`runs/render/fallback5/measure.py` prints the PSNR, backdrop and alignment
table against `fallback4` (the two scripts live with the data, not in the
repo).
