# `.psplat` scene format, version 1

`.psplat` is the runtime scene file written by C5 and read by the renderer. It
is a small fixed header followed by five fp16 struct-of-arrays blocks. One file
holds one reconstructed photo. All integers and floats are little-endian.

At 1,179,648 Gaussians (SHARP's output for one photo) the file is 33,030,224
bytes: 80 header + 1,179,648 × 14 × 2. The same scene in fp32 is 66 MB.

```
offset  size  field
0       8     magic "PSPLAT\0\0"
8       4     version (uint32) = 1
12      4     count (uint32), number of Gaussians
16      4     values_per_splat (uint32) = 14
20      4     layout (uint32) = 1
24      12    bounds_min (3 × float32, xyz metres)
36      12    bounds_max (3 × float32, xyz metres)
48      4     focal_px (float32, 0 when unknown)
52      4     image_width (uint32, 0 when unknown)
56      4     image_height (uint32, 0 when unknown)
60      20    reserved, zero
80      ...   attribute arrays
```

`magic` is compared byte-for-byte. `version` must be exactly 1; readers reject
other versions. `values_per_splat` must be 14 and `layout` must be 1; these
exist so a future layout can bump the version instead of silently misreading.
`bounds_min`/`bounds_max` are the axis-aligned bounds of `position` and are
informational (the loader may still scan positions). `focal_px` and the image
size are copied from the scene metadata for the renderer/UI and are not used by
the loaders.

## Attribute arrays

The arrays follow the header in this fixed order, each tightly packed, with no
padding between blocks. Each value is IEEE 754 binary16 (fp16), little-endian.

| Order | Array | Values per Gaussian | Total values |
| --- | --- | --- | --- |
| 1 | position | 3 (x, y, z) | 3 × count |
| 2 | log_scale | 3 (x, y, z) | 3 × count |
| 3 | rotation | 4 (w, x, y, z) | 4 × count |
| 4 | sh_dc | 3 (R, G, B) | 3 × count |
| 5 | opacity_logit | 1 | count |

Conventions match the standard 3DGS `.ply` that SHARP writes (`utils/gaussians.py`
in the reference):

- **position** — metres, OpenCV axes: x right, y down, z forward. No axis flip
  between the reference output and the renderer.
- **log_scale** — `log(sigma)` per axis, in metres. Not the activated scale.
- **rotation** — quaternion `(w, x, y, z)`, not necessarily normalised on disk.
  `q` and `-q` are the same rotation; do not compare quaternions elementwise.
- **sh_dc** — SH degree-0 coefficient of the sRGB colour, i.e.
  `(rgb - 0.5) / sqrt(1/4π)`. SHARP emits SH degree 0 only; there are no
  higher-order `f_rest` coefficients. The renderer applies the inverse SH
  constant and the sRGB/linear choice; `.psplat` stores the same sRGB convention
  as the reference `.ply`, so a `.ply` round trip is value-preserving.
- **opacity_logit** — `logit(alpha)`. Producers must clamp `alpha` to
  `(epsilon, 1 - epsilon)` before taking the logit: the reference `.ply` writer
  emits `+inf` when the sigmoid saturates (36 Gaussians in photo01). Readers
  accept the resulting fp16 infinity, but new reconstructions should avoid it.

`float32 -> fp16` conversion is round-to-nearest-even, implemented in
`core/common/half.h` so every platform produces identical bytes. Round-trip
error is bounded by fp16's ~11-bit mantissa (about 2e-3 relative).

## Validation rules

A reader must reject a file when any of these fail:

1. size ≥ 80 bytes and `magic` matches;
2. `version == 1`, `values_per_splat == 14`, `layout == 1`;
3. size equals `80 + count × 28` exactly (truncated and trailing data are both
   errors).

`count` is a uint32, so the maximum file is about 120 GB; implementations should
apply their own memory sanity limit before allocating.

## Scene folder

C5 stores one folder per photo:

```
<photo>/
  original.jpg    full-resolution input, copied verbatim
  thumb.jpg       gallery thumbnail, copied verbatim
  scene.psplat    this format
  meta.json       backend, intrinsics, timing, format version
```

`meta.json` (version 1):

```json
{
  "format_version": 1,
  "backend": "sharp",
  "intrinsics": {
    "width": 1280,
    "height": 873,
    "fx": 1074.2907,
    "fy": 1074.2907,
    "cx": 640.0,
    "cy": 436.5
  },
  "seconds_taken": 12.5,
  "splat_count": 1179648
}
```

`format_version` is the `.psplat` version written by the same producer.
`intrinsics` refers to the original image: `fx`/`fy` are `focal_px` from EXIF
(or the 30 mm default) and `cx`/`cy` are the principal point, normally the image
centre. `seconds_taken` is the reconstruction wall time. `splat_count` must
match the `.psplat` header when non-zero. Unknown keys are ignored by the
reader; missing required keys are an error.

## Standard `.ply` import/export

`import_ply` / `export_ply` read and write binary little-endian 3DGS `.ply`.
The importer maps these 14 vertex properties by name, so property order may
differ: `x y z f_dc_0 f_dc_1 f_dc_2 opacity scale_0 scale_1 scale_2 rot_0 rot_1
rot_2 rot_3`. All must be `float`/`float32`. `f_rest_*` and any other vertex
properties (normals, extra SH bands) are skipped, but they still count towards
the vertex stride. Extra elements after `vertex` (`intrinsic`, `extrinsic`,
`image_size`, `frame`, `disparity`, `color_space`, `version`, as written by
SHARP) are ignored; the `vertex` element must come first. ASCII PLY is rejected.

The exporter writes SHARP's order (`x y z f_dc_0..2 opacity scale_0..2
rot_0..3`) with no extra elements, so a `.psplat -> .ply -> .psplat` cycle is
lossless apart from fp16 quantisation on the `.psplat` side.

## Compatibility notes

- Version 1 is the only version. Add fields by appending to the reserved header
  area and bumping `version` only if the attribute layout changes.
- The file is deliberately not compressed; the renderer maps or streams it
  directly. 1.18 M Gaussians are 33 MB, within the 512 MiB viewing budget.
- `docs/research/sharp-reference.md` §3 (focal), §9 (attributes, logit
  encoding, quaternion sign instability) and §11 (colour pipeline, opacity
  clamp) are the source of the conventions above.
