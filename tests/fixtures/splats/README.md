# Splat import fixtures

The readers are covered by byte vectors written in `tests/splat_import_test.cpp`
and `tests/splat_thinning_test.cpp`, so this folder is empty on purpose: no
splat file is stored in the repository (the largest is tens of megabytes and
none of them is redistributable).

Drop a real file here to exercise a reader against an exporter's own output.
Every test that needs one skips when it is absent, which is the repo's rule for
external data (`tests/data_dir.h`):

| file | what it proves |
| --- | --- |
| `spz_v4.spz` | the v4 header, TOC and zstd streams (Niantic `spz` v4 writer) |
| `supersplat.compressed.ply` | SuperSplat/PlayCanvas `splat-transform` output, 18-property chunks |
| `antimatter15.splat` | a scene converted by antimatter15's `convert.py` |

Keep any file added here under 64 KB (a cropped or hand-reduced scene), and
record its origin and licence next to it.
