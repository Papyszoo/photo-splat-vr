# tools/golden — SHARP reference stage dumps

`dump_golden.py` runs Apple's SHARP predictor (the PyTorch reference) on photos and
writes every stage of the inference graph to `.npy`, plus the reference `.ply`.
These files are the parity oracle for the C3 ggml port and for `tools/parity`.

Everything it produces lives **outside the repo**, under `$SPLAT_DATA_DIR`
(default: the repo's parent directory, see `AGENTS.md`); the golden root is
`$SPLAT_DATA_DIR/golden/`. Tensors and weights are never committed;
`.gitignore` already excludes `*.npy`, `*.npz`, `*.pt`. The scripts below use

```bash
DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
```

so every path is relative to the data directory, never to a personal one.

## Reference environment

The reference is Apple's `ml-sharp` (clone into `ref/ml-sharp`, outside the
repo). It needs torch, timm, pillow, pillow-heif, plyfile, numpy, scipy —
but **not** `gsplat`, which is only used for the CUDA trajectory renderer. Note
that `sharp.cli.predict` imports the renderer at module import time, so this
script deliberately imports `sharp.models` / `sharp.utils` directly instead of
using the `sharp` CLI.

```bash
git clone --depth 1 https://github.com/apple-aiml-research/ml-sharp \
    "$DATA"/ref/ml-sharp
python3.12 -m venv "$DATA"/ref/venv-sharp
V="$DATA"/ref/venv-sharp/bin

$V/pip install torch==2.8.0 torchvision==0.23.0 \
    --index-url https://download.pytorch.org/whl/cpu
$V/pip install timm==1.0.20 pillow==11.3.0 pillow-heif==1.1.1 plyfile==1.1.2 \
    click==8.3.0 imageio==2.37.0 numpy==2.3.3 scipy==1.16.2 matplotlib==3.10.6
$V/pip install -e "$DATA"/ref/ml-sharp --no-deps
```

The checkpoint comes from the official URL (spec rule 2), into
`$SPLAT_DATA_DIR/weights/`:

```bash
curl -L -O https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt
shasum -a 256 sharp_2572gikvuh.pt
# 94211a75198c47f61fca7d739ba08a215418d8d398d48fddf023baccc24f073d
# 2,809,738,232 bytes
```

## Run

```bash
cd <repo>
DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
V="$DATA"/ref/venv-sharp/bin

# one photo
$V/python tools/golden/dump_golden.py \
    --checkpoint "$DATA"/weights/sharp_2572gikvuh.pt \
    --out "$DATA"/golden \
    --photos "$DATA"/golden/inputs/photo02_street.jpg \
    --device cpu

# the whole set
$V/python tools/golden/dump_golden.py \
    --checkpoint "$DATA"/weights/sharp_2572gikvuh.pt \
    --out "$DATA"/golden \
    --photos "$DATA"/golden/inputs/*.jpg \
    --device cpu --skip-existing
```

| Flag | Meaning |
| --- | --- |
| `--device` | `auto` (default), `cpu`, `mps`, `cuda`. **Use `cpu`** for the golden set: it is bit-reproducible. `mps` is ~10× faster (10.5 s vs ~90 s) but differs by up to 1.8e-3 relative per element — fine for eyeballing, not for a parity baseline. |
| `--skip-existing` | keep folders that already have a `reference.ply` |
| `--out` | golden root; one folder per photo, id = input file stem |

Cost per photo: ~90 s and ~9 GiB peak RSS on CPU fp32, ~2 GB of `.npy` on disk.
One process at a time (16 GiB host).

## Output layout

```
<out>/RUN.json                     checkpoint, sha256, device, per-photo timings
<out>/SOURCES.md                   photo provenance + licences
<out>/inputs/                      the source JPEGs
<out>/<photo_id>/000_meta.json     f_px, image size, stage shapes, attribute ranges, timings
<out>/<photo_id>/010_image_uint8.npy            HxWx3 uint8   after EXIF rotation, before resize
<out>/<photo_id>/011_preprocess_out.npy         1x3x1536x1536 f32  [0,1], bilinear align_corners=True
<out>/<photo_id>/020_encoder_in.npy             1x3x1536x1536 f32  [-1,1]  (2x-1)
<out>/<photo_id>/021_encoder_out_00..04.npy     256/256/512/1024/1024 channels
<out>/<photo_id>/030_depth_decoder_out.npy      1x256x768x768
<out>/<photo_id>/031_depth_head_out.npy         1x2x1536x1536   disparity
<out>/<photo_id>/040_depth_align_{out,map}.npy  1x2x1536x1536   out == in, map all ones
<out>/<photo_id>/041_monodepth.npy              1x2x1536x1536   metric depth (m)
<out>/<photo_id>/050_gaussian_decoder_in.npy    1x5x1536x1536   image + normalised disparity
<out>/<photo_id>/051_init_base_*.npy            base values (mean_x/y, inv_z, scale, quat, colour, opacity, global_scale)
<out>/<photo_id>/060_gaussian_decoder_out_{texture,geometry}.npy  1x32x768x768
<out>/<photo_id>/070_delta_values.npy           1x14x2x768x768  mean(3) scale(3) quat(4) colour(3) opacity(1)
<out>/<photo_id>/080_composer_*.npy             Gaussians in NDC, 1x1179648xC
<out>/<photo_id>/090_unproject_*.npy            metric Gaussians + intrinsics_resized
<out>/<photo_id>/reference.ply                  3DGS PLY, byte-for-byte what `sharp predict` writes
```

Shapes are `NCHW`; the 5D Gaussian stages are `B x C x layers x H x W` until the
composer flattens to `B x (layers*H*W) x C`.

## Comparing a port against the golden set

- Compare per stage, in order: preprocess → encoder → depth decoder → depth head
  → initialiser → Gaussian decoder → prediction head → composer → unprojection.
- `040_*` proves the depth-adjustment U-Net is not used at inference; a port may
  skip it (and should drop its 2.0 M params at conversion time).
- Do **not** compare quaternions elementwise: `q` and `-q` are the same rotation
  and the reference's scipy-based SVD picks signs unstably (CPU vs MPS already
  differ by 1.42 on the same photo). Compare `R(q) diag(σ²) R(q)ᵀ`, or
  canonicalise the sign first.
- A raw per-element 1e-3 relative tolerance is unrealistic for the
  pre-activation deltas (`070_*`, up to 8.7 relative between two PyTorch
  backends). Use max-abs relative error where `|ref| > 1e-2` plus a p99.9 bound,
  and judge end-to-end quality by PSNR of rendered images (spec: ≥ 35 dB).
- The `.ply` is sRGB SH-DC with log-scales and logit-opacities; the composer
  tensors (`080_*`) are linearRGB with plain scales/opacities.

All of the above is measured in `docs/research/sharp-reference.md`, including
the full op inventory and the checkpoint's pickle layout for the C1 unpickler.

## Depth Anything V2 Small (C4 fallback)

`dump_da2_golden.py` is the same idea for the fallback network: it runs the
official reference (`github.com/DepthAnything/Depth-Anything-V2`, cloned into
`ref/Depth-Anything-V2`) with the official Apache-2.0 Small safetensors
checkpoint and dumps the stages the ggml port has to match.

```bash
DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
git clone --depth 1 https://github.com/DepthAnything/Depth-Anything-V2 \
    "$DATA"/ref/Depth-Anything-V2
V="$DATA"/ref/venv-sharp/bin
$V/pip install opencv-python-headless        # the reference transform uses cv2

$V/python tools/golden/dump_da2_golden.py \
    --weights "$DATA"/weights/dav2s.safetensors \
    --repo "$DATA"/ref/Depth-Anything-V2 \
    --out "$DATA"/golden/da2 \
    --photos "$DATA"/golden/inputs/*.jpg \
             "$DATA"/golden/immichvr/4.jpg \
             "$DATA"/golden/immichvr/5.jpg
```

The HF checkpoint names its tensors `backbone.*` / `neck.*` / `head.*`; the
script maps them onto the reference graph (`pretrained.*` / `depth_head.*`,
fusing the four separate q/k/v projections into one `qkv`). It fails loudly on
an unmapped tensor, and `load_state_dict(strict=True)` catches shape drift.

Layout per photo (`<out>/<photo_id>/`, ~5 s and ~3 GiB RSS on CPU):

```
010_input.npy            1x3xHxW  normalised network input (518 short side)
011_image_rgb.npy        HxWx3 uint8, EXIF-rotated, as loaded
020_patch_embed.npy      1 x npatch x 384
021_pos_embed.npy        1 x (1+npatch) x 384, bicubic-interpolated
030_block_{02,05,08,11}.npy   1 x (1+npatch) x 384
040_tap_patch_XX.npy / 040_tap_cls_XX.npy   post-norm tap tokens
050_layer_XX.npy         1 x C x h x w  (reassemble: V4, V8, V16, V32 on the patch grid)
060_path_{4..1}.npy      1 x 64 x h x w (FeatureFusionBlock outputs, deep -> shallow)
061_conv1.npy            1 x 32 x h x w (head conv before the upsample to input res)
070_depth_net.npy        HxW relative inverse depth (larger = closer)
071_depth_full.npy       HxW resized to the photo (infer_image behaviour)
```

The HF fusion stage is ordered deepest-first: `neck.fusion_stage.layers.0`
is the reference `refinenet4`. That mapping matters (all four fusion blocks
have identical shapes, so a wrong order still loads silently) — verified
against `transformers` `DepthAnythingForDepthEstimation` on the same weights
(correlation 0.9998, mean |diff| 0.012 from preprocessing differences).
