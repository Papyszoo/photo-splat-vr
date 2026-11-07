# Importing splat files

An imported scene is an already-converted gaussian-splat file (from SuperSplat,
Luma, Niantic's `spz` tools, antimatter15's converter, …) turned into the app's
own `.psplat` scene folder. There is no photo and no reconstruction model: the
reader decodes the file, the scene is thinned to the renderer's budget, and the
result is written as a normal scene the gallery, the playlist and the viewer
read.

## Formats the app reads

| Format | Extensions | How it is recognised | Notes |
| --- | --- | --- | --- |
| `.psplat` | `.psplat` | magic `PSPLAT\0\0` | the app's own runtime scene, kept as is |
| 3DGS `.ply` | `.ply` | header `ply` + float vertex properties | the existing SHARP/3DGS reader; `f_rest_*` is ignored |
| `.splat` | `.splat` | extension + a multiple of 32 bytes | 32-byte records, no header |
| `.spz` v1–v3 | `.spz` | gzip magic `1f 8b` | 16-byte header inside the gzip stream; v1 stores fp16 positions, v2/v3 24-bit fixed point |
| `.spz` v4 | `.spz` | plaintext magic `NGSP`, version 4 | 32-byte header, TOC, one zstd frame per stream |
| `.compressed.ply` | `.compressed.ply` | `ply` header with `element chunk` and `packed_position` | SuperSplat/PlayCanvas quantised PLY |

Content decides first: a `.ply` whose header carries the chunk element is read
as a compressed PLY, whatever its name. Only `.splat` has no magic, so its
extension and the fixed record size are the signal.

Every reader returns the same in-memory scene: positions in metres on the
OpenCV axes (x right, y down, z forward), `log_scale` = ln(sigma) per axis,
`rotation` as a unit quaternion (w, x, y, z), `sh_dc` as SH degree-0 RGB and
`opacity_logit` = logit(alpha). spz is stored right/up/back, so its positions
are flipped to (x, −y, −z) and its quaternions to (w, x, −y, −z). SH degree 1
and above is dropped: the renderer consumes degree 0 only.

`.sog` and `.ksplat` are not read (both need 350–550 lines of reader for formats
the target tools do not export by default); they are in `docs/BACKLOG.md`.

## Guards, before any allocation

`core/splat_import/splat_import.h` holds the three numbers:

```cpp
inline constexpr uint32_t kMaxSceneSplats  = 2'000'000;  // thinning target
inline constexpr uint32_t kMaxSourceSplats = 8'000'000;  // refuse larger sources
inline constexpr uint64_t kMaxSplatFileBytes = 512ull * 1024 * 1024;
```

A file larger than 512 MiB is refused from its size, before a byte is read. A
header that declares more than 8,000,000 splats is refused from the header,
before the scene buffers are sized (for `.splat` the file size *is* the header;
for `.psplat` and `.ply` the count field is checked before their vertex payload
is read).
Both messages name the limit, and the job row shows them.

## Thinning

2,000,000 splats is the renderer's budget, not a UI preference. A viewed scene
costs 84 bytes per splat on the GPU — 28 B of fp16 attributes, a 48 B instance
record, and 4 B key + 4 B value for the radix sort — plus, transiently, a 56 B
fp32 scene while the file is uploaded and the 28 B file bytes themselves.
2,000,000 splats is therefore 168 MiB + 112 MiB + 56 MiB ≈ 336 MiB, inside the
512 MiB "viewing one scene" budget of `docs/spec.md`, with room for the stereo
eye targets (~33 MiB), the overlay and the preload slot. 4,000,000 would be
~680 MiB and blow it.

An import larger than the budget keeps the splats that contribute most:

```
importance = exp(log_scale.x + log_scale.y + log_scale.z) * sigmoid(opacity_logit)
```

— the Gaussian's volume times its coverage. `std::nth_element` picks the top
2,000,000 (ties keep the lower index) and the survivors are written back in
their original index order, so the same file always produces the same scene and
an exporter's own ordering survives where possible. The queue row says what
happened: `imported 1,234,567 splats` or `thinned 4,000,000 to 2,000,000
splats`, and `meta.json` keeps both numbers:

```json
"origin": { "format": "spz", "count": 4000000 }
```

`count` is the source file's count, `splat_count` the thinned one; the format
token is `splat`, `spz`, `compressed_ply`, `ply` or `psplat`.

## The scene folder an import writes

`original.jpg` is not written (there is no photo), so the folder holds
`scene.psplat`, `thumb.jpg` and `meta.json`. `thumb.jpg` is a deterministic
vertical gradient of the scene's mean colour (0.94× at the top, 1.06× at the
bottom, so its mean is the scene's colour) — a real render needs the renderer,
and the gallery only needs a recognisable tile. The gallery lists such a folder
like any other scene: a scene folder is a directory holding `scene.psplat`.

`meta.json` also carries the source (`"origin": {"format": "spz", "count": N}`)
and, once the user scales the scene in the viewer, the saved view
(`"view": {"scale": s}`) beside it. Rotation is not a viewer feature: **L3/R3**
on the scene's file entry rewrites the scene's own `scene.psplat`,
`original.jpg`, `thumb.jpg` and intrinsics in `meta.json` (DECISIONS.md,
"Change 2026-11-05"), so the turn is in the files and needs no `view` entry.
See `docs/render.md` and `Scaled imported scenes` below.

## Scaled imported scenes

An imported splat has no capture camera, so its natural size is whatever the
exporting tool chose: a `.ply` from a photogrammetry tool can land metres wide
or at a fraction of a centimetre. The viewer's L2/R2 scale therefore belongs to
*that scene* and is stored with it, in `meta.json`'s `"view"` object:

```json
"view": { "scale": 0.42 }
```

A converted scene keeps the older rule instead: a scene switch keeps the
session's scale and offset (no `"scale"` key is written), because its size is
the capture camera's and does not need a per-scene answer. Deleting the `"scale"`
key (or setting it to 1) returns an imported scene to the file's own size. A
`"rotation"` a pre-0.5 build left in `"view"` is ignored on load and dropped by
the next save; rotation lives in the scene files now.

## In the photo picker

Every picker tab is a folder browser (Console, Inbox, each USB stick, each
network location): a folder shows its subfolders as folder tiles, then its
photos, then its splat files, as tiles labelled `SPLAT · spz` with a `SPLAT`
badge and no thumbnail (decoding one just to draw a preview would import it
twice). Cross on one queues its import into the folder the picker was opened
for, and the bar says `Import` rather than `Select`: the tile never joins the
photo basket. The toast reads `Importing <name>`. Files are picked one by one,
as in a file browser; there is no search and no folder-level import, and Square
on a folder tile does nothing. The file is read through the file-system layer,
so a splat file on a network share imports like one on a stick. Imports and conversions share
the one queue and run one at a time, but an import loads no model, so it needs
no GGUF and does not pause a running viewer.
