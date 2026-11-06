# Photo fixtures

Small, committed photos the format tests decode. Every file is generated once by
the command next to it, and the SHA-256 is recorded so a re-generated file that
differs is noticed in review. Nothing here is a camera image.

## tiny_source.png

- 16x16 RGB, an 8-step horizontal/vertical gradient with a constant blue
  channel (the reference the WebP, HEIC and AVIF means are compared against).
- Generated with a 20-line Python script using only `zlib` and `struct`
  (PNG signature, one IDAT of filter-0 rows, IEND); the script is not kept
  because the file is 459 bytes and fully described by its SHA-256.
- SHA-256: `cc1942673c3eb1154d1a68d99e286a1899d2d6854f4c743eb9f7f345f652a2d7`

## tiny.webp

- 16x16 lossless WebP, 52 bytes (`cwebp -lossless -resize 16 16`, from
  `tiny_source.png` in this folder). Lossless, so the test's per-channel mean
  must match the PNG exactly up to the ±2 tolerance it allows.
- Command: `cwebp -lossless -resize 16 16 tiny_source.png -o tiny.webp`
- Tool: libwebp 1.6.0 (`cwebp` from Homebrew, same version as
  `third_party/libwebp`).
- SHA-256: `8ed36786b3c9babf66dccd1e82ff826113741614c90a670fac37e0977abbc4ea`

## tiny.heic

- 16x16 HEIC, 1087 bytes, from `tiny_source.png` through macOS ImageIO:
  `sips -s format heic -s formatOptions 100 tiny_source.png --out tiny.heic`
- macOS writes an `irot` property (angle 0 here), which is why the decoder takes
  the handle's post-transform size instead of the `ispe` box.
- SHA-256: `b0e12faf335386e4a3cc69920585e6ce5dc07eb5ad8be6b4b0c4ce56fb8501b9`

## tiny.avif

- 16x16 AVIF, 451 bytes. `avifenc` is not installed on the build host, so it
  was written by Pillow's bundled libavif:
  `python3 -c "from PIL import Image; Image.open('tiny_source.png').convert('RGB').save('tiny.avif', format='AVIF', quality=100, speed=0)"`
  (Pillow 11.3.0, `features.check('avif')` true). Same source, same means.
- SHA-256: `bd5335f52ca465d4aee788fac996f1abee1d9622c6268cd02c3fc4c802eb8587`

## wide.heic

- 32x16 HEIC, 1280 bytes, from a 32x16 gradient PNG (the same script, 32x16
  rows) through the same `sips -s format heic -s formatOptions 100` call.
- It stores `irot` with angle 0. `tests/image_heif_test.cpp` sets that angle
  byte to 1 in memory and checks the decode is 16x32: the container transform
  is applied once, and the `ispe` size (32x16) is only the guard's size.
- SHA-256: `2be4bfa96a6eda9c65452222da05f6fe777d4e219c556611e99ff88dcb8a9b40`

## A HEIC with an EXIF orientation and no irot

There is none. Authoring one needs an HEVC *encoder* (x265 or VideoToolbox) and
neither build host nor payload ships one: this repository only decodes HEIC.
The double-rotation rule ("a container transform wins, the EXIF tag is not
applied on top of it") is therefore tested as a function,
`image::heif_orientation_to_apply()`, next to the real-file transform test above.

## Decoding in the tests

`tests/image_decoders_test.cpp` builds its TIFFs from bytes in the test (no
fixture) and reads `tiny.webp`/`tiny_source.png` here;
`tests/image_heif_test.cpp` reads `tiny.heic`, `tiny.avif`, `wide.heic` and the
same source PNG. Both find this folder through
`__FILE__`, the same way `tests/sharp_converter_fp32_test.cpp` finds its model
checkpoint, so the tests work from any build directory.
