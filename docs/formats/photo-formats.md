# Photo formats

One list decides what a photo is, and one decoder path handles every file:
`core/image/format.h` (the extensions and the magic-byte sniff) and
`core/image/decode.cpp` (the guards, the dispatch, EXIF orientation, the manual
quarter turns). Every component that used to filter extensions itself — the
gallery scan, the inbox watcher, the capture helper, the upload store and the
PS5 bench converter — calls `is_photo_file()` now.

## Extensions

| extension | format | decoder |
| --- | --- | --- |
| `.jpg` `.jpeg` | JPEG | stb_image (baseline and progressive) |
| `.png` | PNG | stb_image (8-bit, any colour type) |
| `.bmp` | BMP | stb_image |
| `.gif` | GIF | stb_image, first frame |
| `.tga` | TGA | stb_image (no magic bytes: the extension is the only signal) |
| `.webp` | WebP | libwebp (lossy, lossless, alpha composited on white) |
| `.tif` `.tiff` | TIFF | libtiff (none/LZW/PackBits) |
| `.heic` `.heif` | HEIF | libheif + libde265 (HEVC) |
| `.avif` | AVIF | libheif + libaom (AV1) |
| `.dng` `.cr2` `.cr3` `.nef` `.arw` `.raf` `.orf` `.rw2` `.pef` `.srw` | RAW | LibRaw (embedded preview first, half-size demosaic second) |

`.jxr` (HDR screenshots) stays listed by the capture screen and is refused with
a message: there is no decoder for it.

Content wins over the name: `sniff_image_format()` reads the magic bytes, and
the upload store names a stored file after what it found, not after what the
client called it. The one exception is the RAW extensions, where the content is
a plain TIFF container (`.nef` `.arw` `.orf` `.rw2` `.pef` `.srw`) or a
container with no known brand (`.cr3`, `.raf`): the extension is passed to
`decode_photo()` as `DecodeOptions.hint` so those reach LibRaw instead of
libtiff. A `.dng` whose bytes are a PNG is still refused with "not a RAW file".

## Limits

`image::DecodeOptions` defaults, checked on the header **before** any
allocation (JPEG SOF, PNG IHDR, GIF logical screen descriptor, BMP info header,
TIFF IFD0, WebP VP8X/VP8/VP8L, ISO-BMFF `ispe` of the primary item via
`pitm`/`ipma` — not the first `ispe` in the file, which may be a thumbnail):

- `max_pixels = 64'000'000` — "photo is 70000x1000 (70.0 MP); the limit is
  64 MP - export a smaller copy".
- `max_edge = 16'384` — the longest side, for a panorama that is under the
  pixel budget but too wide for one texture.
- `max_bytes = 512 MB` — checked against the file size, before the body is read.

Nothing is downscaled: an over-limit photo is refused with the numbers. A
format with no readable header (TGA) is checked after decoding.

## Orientation

EXIF orientation 1..8 is applied to the pixels, and the intrinsics
(`cx`, `cy`, and `fx`/`fy` when the axes swap) go through the *same* 2x3 pixel
map (`core/image/orientation.h`), so a scene can never describe a differently
oriented photo. A manual 90° turn is a file action on the picker entry
(**L3/R3**, DECISIONS.md "Change 2026-11-05"): a JPEG or baseline TIFF has its
Orientation tag rewritten so the pixels are untouched, and any other format
(RAW, HEIC, AVIF, PNG, WebP, a read-only share) keeps its source bytes and the
turn is stored in `<state>/photo-turns.json`. The turn composes after the EXIF
turn and reaches the job once, as the `"rotate"` quarter count.

Each decoder that already turns its own output marks the orientation as
consumed, so `decode_photo()` never applies it a second time: libheif applies
`irot`/`imir` (a file with a transform uses that, not its EXIF tag), libtiff is
told the stored raster is top-left so it does not normalise, and LibRaw applies
the camera flip in the demosaic path while the embedded preview gets its own
EXIF orientation. The TIFF decoder therefore returns the *stored* pixels, and
the one map above turns them.

`original.jpg` in a finished scene folder is the source JPEG byte for byte only
when the file is a JPEG, its EXIF orientation is 1 and the manual turn is 0.
Everything else is re-encoded from the oriented pixels, so the stored original
matches the scene.

## RAW

| extension | camera family |
| --- | --- |
| `.dng` | Adobe DNG, and every phone/Leica/Pentax DNG |
| `.cr2` `.cr3` | Canon |
| `.nef` | Nikon |
| `.arw` | Sony |
| `.raf` | Fujifilm |
| `.orf` | Olympus / OM System |
| `.rw2` | Panasonic |
| `.pef` | Pentax |
| `.srw` | Samsung |

LibRaw (`third_party/libraw`, 0.22.3) decodes them, in this order:

1. **Embedded preview first.** Nearly every RAW carries a full- or half-size
   JPEG of the frame; `libraw_unpack_thumb()` gets it and the JPEG decoder
   above decodes it, so the scene gets a fully processed image with the
   camera's colour, not a demosaiced mosaic. A preview whose long edge is
   below 512 px is ignored. `DecodeInfo.raw_preview` is `true` and the note
   reads "raw preview (<make>)".
2. **Half-size demosaic otherwise.** `half_size = 1`, `output_bps = 8`,
   `libraw_unpack()`, `libraw_dcraw_process()`,
   `libraw_dcraw_make_mem_image()` → RGB8. The note reads
   "half-size demosaic (<make>)" and `raw_preview` is `false`.

Full-size demosaic, RAW colour profiles beyond LibRaw's default and DNG writing
are out of scope. LibRaw is compiled with `-DLIBRAW_NOTHREADS` and **without**
`USE_ZLIB`, `USE_JPEG` and `USE_LCMS` (the payload SDK has none of them), so a
floating-point DNG — which is Deflate-compressed — is refused by LibRaw with its
own message instead of decoded.

The intrinsics come from the file's own EXIF (`photo_intrinsics`, which reads
the TIFF IFD0 of a DNG), so a DNG keeps its focal length. A RAW with no EXIF
focal falls back to the 30 mm default like every other photo.

## Licensing

The decoders are the libraries in `THIRD_PARTY_NOTICES.md`: libwebp
(BSD-3-Clause), libtiff (libtiff licence), libheif and libde265 (LGPL-3.0),
libaom (BSD-2-Clause plus the AOM patent licence) and LibRaw (LGPL-2.1, the
CDDL alternative is not taken).
