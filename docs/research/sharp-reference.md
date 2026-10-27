# SHARP reference: licensing, checkpoint, forward pass, op inventory, golden tensors

Task C research output. Everything here was measured on this host unless marked
otherwise. Citations are `path:line` relative to the reference checkout at
`$SPLAT_DATA_DIR/ref/ml-sharp/src/sharp/` unless stated. `$SPLAT_DATA_DIR` is
the data directory from `AGENTS.md` (default: this repo's parent directory);
no personal absolute path belongs in this document.

| Item | Value |
| --- | --- |
| Reference repo | `github.com/apple-aiml-research/ml-sharp`, commit `aed6527` (2026-09-11) |
| Checkpoint | `https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt` (URL from `cli/predict.py:36`) |
| Checkpoint size | 2,809,738,232 bytes (2.617 GiB) |
| Checkpoint SHA-256 | `94211a75198c47f61fca7d739ba08a215418d8d398d48fddf023baccc24f073d` |
| Params | 702,305,169 (all fp32); 655,515,838 used at inference |
| Environment | macOS 26.5 arm64, 16 GiB, Python 3.12.13, torch 2.8.0 (CPU + MPS), timm 1.0.20 |
| Reference run | 10.5 s wall / 5.6 GiB peak RSS on MPS; 117 s / 9.2 GiB on CPU fp32 (see §4) |
| Golden set | `$SPLAT_DATA_DIR/golden/`, 3 photos, 5.9 GB, CPU fp32, bit-reproducible |

---

## 1. Licensing, and what it means for spec rule 7

Three licence files, with three different jobs.

| File | What it covers | Key terms |
| --- | --- | --- |
| `LICENSE` | The Python code | Apple grants a personal, non-exclusive licence to use, reproduce, modify and redistribute, source or binary, with or without modification. Redistributing the software *unmodified and in its entirety* must keep the notice. No Apple name/trademark use for endorsement. **No patent grant** ("no other rights ... including but not limited to any patent rights"). AS IS. Not OSI-approved. |
| `LICENSE_MODEL` | The checkpoint and any derivative | Use, copy, modify, distribute and create Model Derivatives **only for Research Purposes**; explicitly excludes "product development or use in any commercial product or service". Redistribution requires shipping the agreement text, the notice "Apple Machine Learning Research Model is licensed under the Apple Machine Learning Research Model License Agreement.", and derivatives must be *clearly identified as such with the changes disclosed*. Revocable; terminate ⇒ delete all copies. California law. |
| `ACKNOWLEDGEMENTS` | Third-party code inside the repo | Lists **TIMM** (Apache-2.0, Ross Wightman) only. Nothing else. |

**Rule 7 verdict (reimplement, do not port closely).** Two independent reasons:

1. `LICENSE` grants modification/redistribution but is *not* GPL-3.0-compatible in
   practice: no patent grant, plus an endorsement restriction, and no explicit
   statement permitting relicensing under a copyleft licence. Copying
   `models/*.py`, `utils/*.py` or `cli/*.py` into our GPL-3.0-or-later tree would
   put the whole app's licensing in question.
2. `LICENSE_MODEL` is the binding constraint on the artifact anyway: the
   converted GGUF is a Model Derivative (format and dtype change, disclosed), the
   app must never ship it (spec rule 1), and Research Purposes excludes product
   development — which is exactly why the permissively licensed fallback backend
   stays the default (spec rule 8).

So: read the code for behaviour (shapes, constants, activation order, epsilon
values), and write the ggml implementation from the paper + this document. The
only third-party code we may reuse verbatim is timm's (Apache-2.0, compatible
with GPL-3.0-or-later), and only if we actually take any of it — the ViT here is plain
timm so we do not need to.

Nothing in `weights/` or `golden/` may enter the repo. `.gitignore` already
covers `*.pt`, `*.npy`, `*.npz`.

---

## 2. Checkpoint: size, parameters, SH degree

`sum(v.numel())` over the state dict, and `torch.load(..., weights_only=True)`.

| Group | Params | Notes |
| --- | --- | --- |
| `monodepth_model` | 692,505,860 | Depth Pro dual-ViT-L/16 + DPT decoder + disparity head |
| ├ `encoder` (SPN incl. 5 upsamplers) | 672,440,994 | 2× ViT-L/16 (326,126,929 each) + 20.19M upsamplers, incl. 44,774,050 dead head |
| ├ `decoder.*` | 19,667,200 | `convs` 6,488,064 + `fusions` 13,179,136 |
| └ `head.*` | 397,666 | 2-layer disparity head (32 ch, out 2 layers) |
| `feature_model` (Gaussian decoder) | 7,783,104 | DPT decoder + skip conv + texture/geometry heads |
| `depth_alignment.scale_map_estimator` | 2,015,281 | **present but never run at inference** (§3 Q1) |
| `prediction_head` | 924 | two 1×1 convs (32→6, 32→22) |
| **Total** | **702,305,169** | 2,809,220,676 bytes of tensor data |
| **Used at inference** | **655,515,838** | 2,622,063,352 B fp32 = 2.44 GiB, or 1.22 GiB fp16 |

Dead weight to drop in C1 (171 MiB of fp32 ViT heads + 8 MiB of alignment U-Net):

* `encoder.{patch,image}_encoder.head.weight` (21841, 1024) = 22,365,184 params
  each — the timm ImageNet classifier head, never called because `TimmViT.forward`
  is overridden (`models/encoders/vit_encoder.py:60-84`). `num_classes=21841` is a
  leftover DINOv2/ImageNet-22k value (`models/presets/vit.py:30`).
* `encoder.{patch,image}_encoder.head.bias` (21841,) each.
* the whole `depth_alignment.*` subtree (§3 Q1).

**Output colours are SH degree 0.** The composer emits RGB (3 channels,
`models/composer.py:230-245`) and `save_ply` packs them as the DC term only
(`utils/gaussians.py:190-207, 371-374`): `f_dc = (rgb - 0.5) / sqrt(1/4π)`, no
higher-order coefficients. Colour space: sRGB in, linearRGB inside, converted
back to sRGB at PLY export (comment at `utils/gaussians.py:359-370`).

**Gaussian count.** 1,179,648 per photo = 768² × 2 layers
(`predictor.output_resolution = 1536/2`, `models/predictor.py:194-201`;
`num_monodepth_layers=2`, `models/params.py:199`). Exactly the "roughly 1.2
million" in the spec. Attributes per Gaussian: 3 mean + 3 scale + 4 quaternion +
3 colour + 1 opacity = 14, i.e. 56 B in fp32, 28 B in fp16 — the spec's 34 MB
`.psplat` estimate is right (1.18M × 28 B = 33.0 MB).

---

## 3. The spec's three SHARP open questions

### Q1 — Does the depth-adjustment U-Net run at inference? **No.**

`RGBGaussianPredictor.forward` always calls `self.depth_alignment(monodepth,
depth, decoder_features)` with `depth=None` at inference
(`models/predictor.py:176-180`); `DepthAlignment.forward` then takes the else
branch and returns a **ones** map without touching `scale_map_estimator`
(`models/predictor.py:52-61`). The module is still *constructed*
(`models/alignment.py:22-40` returns a `LearnedAlignment` unconditionally) and its
2,015,281 params are in the checkpoint — which is why `load_state_dict` is
strict-clean — but the U-Net is dead code at inference. The docstring says so
explicitly: it exists so symbolic tracing can exclude the conditional branch
(`models/predictor.py:25-28`).

Measured confirmation: dumped `depth_alignment` output is *bit-identical* to its
input and the alignment map is exactly 1.0 for all three golden photos
(`040_depth_align_map.npy`), and the aten trace contains no `avg_pool2d` from the
U-Net and only the 8 `native_group_norm` calls of the Gaussian decoder heads
(§7). The U-Net is: `UNetEncoder`(steps=4, widths 16/32/64/128/256, group norm 4
groups, avg-pool down) + `UNetDecoder`(4 up steps) + 1×1 conv, `exp` activation
(`models/alignment.py:84-94`); input would be 2 channels of inverse depth.

**C1 action: skip those 156 tensors on conversion. ggml: no code needed.**

### Q2 — Checkpoint size, parameter count, SH degree

2,809,738,232 B / 702,305,169 params (655,515,838 live) / SH degree 0. See §2.

### Q3 — Focal length when there is no EXIF

`utils/io.py:56-66`: prefer `FocalLengthIn35mmFilm` (or `FocalLenIn35mmFilm`);
else `FocalLength`; else **default 30 mm** with a warning. If the value found is
< 10 mm it is assumed not to be a 35 mm-equivalent and multiplied by 8.4
("very crude approximation").

Then `utils/io.py:97-99`:

```
f_px = f_mm * sqrt(w² + h²) / sqrt(36² + 24²)      # 36x24 = 35 mm film
```

EXIF `Orientation` is applied before this (`utils/io.py:44-54`), so w,h are the
rotated dimensions. All three CC0 golden photos have no EXIF → 30 mm:
1280×720 → **1018.29 px**, 1280×873 → 1074.29 px, 1280×964 → 1111.07 px.

Note what the focal length actually does: the network is conditioned on
`disparity_factor = f_px / width` (`cli/predict.py:171`) and
`monodepth = disparity_factor / disparity` (`models/predictor.py:130-131`). Only
the **ratio** `f_px/width` enters the network; the absolute `f_px` is used only
in the final unprojection. A wrong focal length therefore changes metric scale,
not just framing. C2's `std::optional<float> focal_px` is sufficient — the
backend needs nothing else.

---

## 4. Reference inference run

`sharp predict` equivalent: load → 1536² preprocess → forward → unproject →
`save_ply`. One photo (`photo02_street`, 1280×720, no EXIF), no rendering.

| Device | Forward wall time | Peak RSS | Notes |
| --- | --- | --- | --- |
| MPS (warm, median of 5) | **10.5 s** | 5.57 GiB | MPS driver-allocated 10.47 GiB |
| CPU fp32 (torch default threads) | 117 s | 9.2 GiB | golden generation path |
| MPS + golden dump in-process | 9–136 s | 5.55 GiB | dominated by `.npy` writes; not a timing number |

Per-stage on MPS, one stage per process, min of 3 runs (fp32):

| Stage | Time | Share |
| --- | --- | --- |
| Encoder (2× ViT-L over 35 patches) | 5.90 s | 59% |
| Depth decoder (DPT, to 768²) | 1.28 s | 13% |
| Disparity head (up to 1536²) | 0.28 s | 3% |
| Initialiser | 0.01 s | <1% |
| Gaussian decoder (DPT to 768² + heads) | 1.18 s | 12% |
| Prediction head | 0.005 s | <1% |
| Composer | 0.015 s | <1% |
| Unprojection (fp64 SVD, CPU) | 1.30 s | 13% |
| **Sum** | **9.97 s** | matches the 10.5 s total |

Two porting consequences: the encoder is the only thing that really matters for
the 60 s budget, and the **unprojection costs 13% of the whole run because the
reference does a fp64 CPU SVD of 1.18 M 3×3 matrices** (`utils/gaussians.py:151-169`).
It is unnecessary — see §9.

Memory: after a run MPS holds 2.64 GiB of live allocations (weights + live
activations) but the allocator's driver-allocated total is 10.47 GiB, i.e. the
cached peak. The 1536² tensors are the problem: the encoder's first output alone
is 1×256×768×768 = 604 MB fp32, and the DPT decoder output is another 604 MB. The
spec's 8 GiB inference budget is feasible with patch batching but **not** with
all of these live at once in fp32.

Host caveat: this machine also runs other builds. Back-to-back runs showed 10.5 s
clean and up to 79 s under memory pressure (page cache + 12 GB footprint). The
10.5 s figure is the clean-process median.

---

## 5. Exact preprocessing

| Step | Detail | Source |
| --- | --- | --- |
| Decode | PIL; HEIC via `pillow_heif` (`convert_hdr_to_8bit=True`); EXIF orientation 3/6/8 applied | `utils/io.py:35-54` |
| Channels | single-channel → replicated to RGB; alpha dropped (`img[:, :, :3]`) | `utils/io.py:68-74` |
| Scale | `float / 255` → [0, 1] | `cli/predict.py:169` |
| Layout | HWC uint8 → NCHW float32, batch 1 | `cli/predict.py:169` |
| **Resize** | `F.interpolate(size=(1536,1536), mode="bilinear", align_corners=True)` — **anisotropic, no padding, no letterbox, no crop** | `cli/predict.py:173-178` |
| Encoder input normalisation | `AffineRangeNormalizer((0,1) → (-1,1))`, i.e. `2x - 1` | `models/monodepth.py:51-53, 94`, `models/normalizers.py:41-72` |
| Conditioning | `disparity_factor = tensor([f_px / width])`, width = **original** width | `cli/predict.py:171` |
| Second input | `feature_input = 2 * concat(image[0,1], disparity_factor/depth) - 1` (5 channels) | `models/initializer.py:116-125` |

The anisotropic resize looks wrong but is self-consistent: the unprojection uses
intrinsics scaled by `1536/width` and `1536/height` (`cli/predict.py:197-199`), so
the stretch cancels exactly and metric geometry is preserved. Do not "fix" it
into a letterbox.

The pyramid inside the encoder then produces 1536², 768², 384² with bilinear
`align_corners=False` × 0.5 and × 0.25 (`models/encoders/spn_encoder.py:192-203`).

---

## 6. Inference forward pass, with shapes at 1536×1536

Notation: `B` = batch; the encoder runs 35 patches at once, everything after the
merge is `B=1`. Shapes are `NCHW` unless stated. All measured, not derived.

### 6.1 Preprocess → encoder (Sliding Pyramid Network, Depth Pro)

| # | Module | Shapes |
| --- | --- | --- |
| 1 | input | 1×3×1536×1536 float32, [0,1] |
| 2 | affine normalise | 1×3×1536×1536, [-1,1] |
| 3 | pyramid | x0 1536², x1 768², x2 384² |
| 4 | `split` x0, overlap 0.25, patch 384 | 25×3×384×384 |
| 5 | `split` x1, overlap 0.5 | 9×3×384×384 |
| 6 | x2 unchanged | 1×3×384×384 |
| 7 | concat → ViT batch | **35**×3×384×384 |
| 8 | `patch_embed.proj` conv 16×16/16 | 35×1024×24×24 → 35×576×1024 (+CLS → 577) |
| 9 | 24× Block, dim 1024, 16 heads | 35×577×1024 (patch enc.), 1×577×1024 (image enc.) |
| 10 | `merge` intermediate feat. (block 5), 5×5 tiles, padding 3 → 1×1024×96×96; `upsample_latent0`: 1×1 (1024→256) + 3× TConv 2/2 | **1×256×768×768** |
| 11 | `merge` block 11 → 1×1024×96×96; `upsample_latent1`: 1×1 + 2× TConv | 1×256×384×384 |
| 12 | `merge` block 17 → 1×1024×96×96; `upsample0`: 1×1 (1024→512) + TConv | 1×512×192×192 |
| 13 | `merge` 3×3 tiles, padding 6 → 1×1024×48×48; `upsample1`: 1×1 + TConv | 1×1024×96×96 |
| 14 | x2 encodings 1×1024×24×24; `upsample2`: 1×1 + TConv | 1×1024×48×48 |
| 15 | `image_encoder` (2nd ViT-L) on x2 | 1×1024×24×24 |
| 16 | `upsample_lowres` TConv 1024→1024 | 1×1024×48×48 |
| 17 | `fuse_lowres` 1×1 2048→1024 with x2_features | 1×1024×48×48 |

Encoder outputs (5 levels, used by both decoders):

| Level | Shape | Channels | Stride |
| --- | --- | --- | --- |
| 0 (`latent0`) | 1×256×768×768 | 256 | 2 |
| 1 (`latent1`) | 1×256×384×384 | 256 | 4 |
| 2 (`x0`) | 1×512×192×192 | 512 | 8 |
| 3 (`x1`) | 1×1024×96×96 | 1024 | 16 |
| 4 (`lowres`) | 1×1024×48×48 | 1024 | 32 |

ViT detail: 24 blocks, dim 1024, 16 heads × 64, patch 16, img 384, MLP ratio 4
(hidden 4096), GELU, LayerNorm eps 1e-6, LayerScale init 1e-5, qkv bias, learned
absolute pos-embed `(1,577,1024)`, one CLS token, no register tokens, no qk-norm
(`models/presets/vit.py:49-58`; timm `vision_transformer.py` `Block.forward`).
Intermediate features are tapped at block indices **5, 11, 17, 23**
(`models/presets/monodepth.py:18-21`; `models/encoders/vit_encoder.py:77-80`).

Patch merging: `merge(..., padding=3)` trims 3 px at interior tile edges, giving
96² from 5×24 and 48² from 3×24 (`models/encoders/spn_encoder.py:318-368`).

### 6.2 Depth decoder (DPT) + disparity head

| # | Module | Shapes |
| --- | --- | --- |
| 18 | `convs[0]` Identity (256→256) | 768² |
| 19 | `convs[1..4]` 3×3 (256→256, 512→256, 1024→256, 1024→256), bias=False | 384², 192², 96², 48² |
| 20 | `fusions[4]` → 96², `[3]` → 192², `[2]` → 384², `[1]` → 768², `[0]` (no upsample) | each `FeatureFusionBlock2d`: resnet1(x1)+x0, resnet2, TConv 2×2, 1×1 conv |
| 21 | decoder output | **1×256×768×768** |
| 22 | `head` = conv3×3(256→128) + TConv2×2(128→128) + conv3×3(128→32) + ReLU(inplace) + conv1×1(32→2) + ReLU | **1×2×1536×1536** disparity |
| 23 | `monodepth = disparity_factor / clamp(disp, 1e-4, 1e4)` | 1×2×1536×1536 (metres) |

`FeatureFusionBlock2d` is ReLU+conv3×3 (no norm, bias=True), twice, residual;
then a transposed conv 2×2 for ×2 upsampling and a 1×1 conv
(`models/blocks.py:127-210`). `batch_norm=False`, `upsampling_mode="transposed_conv"`
(`models/monodepth.py:125-127`, `models/params.py:157`).

### 6.3 Depth adjustment — skipped (Q1). Identity.

### 6.4 Gaussian decoder

| # | Module | Shapes |
| --- | --- | --- |
| 24 | `init_model` (initialiser, no params) | feature input 1×5×1536×1536; base values 1×1/3×2×768×768 |
| 25 | `image_encoder` skip conv k=2, s=2 (5→128, bias) | 1×128×768×768 |
| 26 | `decoder` DPT: convs 1×1(256→128) + 3×3(256→128, 512→128, 1024→128, 1024→128), fusions ×4 TConv | 1×128×768×768 |
| 27 | `fusion` with skip features | 1×128×768×768 |
| 28 | `texture_head`/`geometry_head`: 2× residual(GroupNorm 8, ReLU, conv3×3 128→64→128) + ReLU + conv1×1(128→32) + ReLU | 2× (1×32×768×768) |
| 29 | `prediction_head`: conv1×1 32→6 and 32→22, unflatten, cat | **1×14×2×768×768** |

### 6.5 Composer → Gaussians → unprojection

| # | Module | Shapes |
| --- | --- | --- |
| 30 | composer (elementwise, no learned params) | mean 1×1179648×3, singular 1×1179648×3, quat 1×1179648×4, colour 1×1179648×3, opacity 1×1179648 |
| 31 | unproject (SVD) | same shapes, metric space |
| 32 | `save_ply` | 1,179,648 vertices, 66.1 MB |

---

## 7. Op inventory

Aten-level, `TorchDispatchMode` around one real forward at 1536², CPU fp32.
2,541 dispatch calls, 48 unique ops.

| aten op | Count | Example shape | Notes |
| --- | --- | --- | --- |
| `slice.Tensor` | 738 | 1×1024×24×24 | patch split/merge, timm internals |
| `view` | 499 | 35×577×3072 | attention head unflatten, conv flatten |
| `t` | 192 | 4096×1024 | every `nn.Linear` transposes its weight per call |
| `addmm` | 192 | (20195,1024)·(1024,3072) | all Linear layers (4 per block × 24 × 2) |
| `add.Tensor` | 147 | 1×128×384×384 | residual adds, pos-embed, bias |
| `mul.Tensor` | 124 | 1×5×1536×1536 | normaliser, LayerScale, delta factors |
| `convolution` | **102** | 1×2048×48×48 | 84 `Conv2d` + 18 `ConvTranspose2d` calls |
| `native_layer_norm` | 98 | 35×577×1024 | 49 per ViT |
| `permute` | 56 | 1×4×2×768×768 | HWC flatten in the composer |
| `relu` | 53 | 1×128×384×384 | DPT decoder + heads |
| `transpose.int` | 50 | 35×1024×576 | attention |
| `unbind.int` | 48 | 3×35×16×577×64 | qkv split |
| `_scaled_dot_product_flash_attention_for_cpu` | **48** | (35,16,577,64) | 24 blocks × 2 encoders; 16 heads × 64 dim |
| `gelu` | 48 | 35×577×4096 | exact (erf), not tanh-approx |
| `cat` | 32 | 1×1×2×768×768 | merge tiles, qkv, composer |
| `unsqueeze` | 27 | 1×1×768×768 | broadcast of base values |
| `div.Tensor` | 10 | | disparity→depth, scale constants |
| `native_group_norm` | **8** | 1×128×768×768 | Gaussian decoder heads only (4 per head) |
| `lift_fresh`, `reciprocal`, `repeat`, `expand` | 7/6/6/4 | | initialiser broadcast |
| `sigmoid` | 4 | 1×3×2×768×768 | colour + opacity activations |
| `rsub`, `log` | 4 | | inverse sigmoid of the base values |
| `clamp` | 3 | 1×3×2×768×768 | colour clamp to [0.01, 0.99] |
| `sub.Tensor` | 3 | 1×5×1536×1536 | `2x-1` normalisers |
| `upsample_bilinear2d` | **2** | 1×3×1536² → 768², 384² | image pyramid (×0.5, ×0.25) |
| `max_pool2d_with_indices` | 2 | 1×1×1536×1536 | `surface_min` disparity per layer |
| `avg_pool2d` | 1 | 1×3×1536→768 | colour initialisation (stride 2) |
| `where` + `le` + `bitwise_not` | 2+1+1 | 1×3×2×768×768 | sRGB→linear piecewise |
| `softplus` | 1 | 1×1×2×768×768 | mean z inverse-depth |
| `select.int` | 1 | 1×14×2×768×768 | opacity channel |
| `relu_` | 1 | 1×32×1536×1536 | in-place ReLU in the disparity head |
| `split_with_sizes` | 1 | 35×1024×24×24 | 25/9/1 patch split |
| `arange.start_step`, `ones_like`, `clone`, `fill_`, `neg`, `le`, `min.dim`, `pow`, `scalar_tensor`, `_local_scalar_dense`, `empty.memory_format` | 1–2 each | | initialiser + composer plumbing |

**No `grid_sample`, no `pixel_shuffle`/`PixelShuffle`, no `softmax` op (it is
inside fused SDPA), no `embedding`, no `batch_norm`, no `instance_norm`, no
`layer_norm` outside ViT, no `einsum`, no `linalg` op inside the traced forward.**

Dtypes: every activation and weight is **float32**; the only non-float32 tensors
that appear are the `int64` indices returned by `max_pool2d_with_indices` (unused)
and the Python floats from `_local_scalar_dense` (a `.item()` in the composer).
Nothing casts to fp16/bf16, and no op runs in double — the fp64 SVD is *outside*
the traced forward (postprocess, §9).

Interpolate modes, all of them: `bilinear, align_corners=False` ×2 inside the
encoder pyramid (×0.5, ×0.25), `bilinear, align_corners=True` ×1 in preprocessing
(before the trace, §5). `nn.Upsample(scale_factor=2)` exists in the UNet/decoder
code but is never reached: the DPT decoders use `upsampling_mode="transposed_conv"`
and the alignment UNet is skipped.

Layer-level counts (module hooks, leaf modules):

| Layer | Calls | Where |
| --- | --- | --- |
| `Conv2d` | 84 | SPN upsamplers, DPT decoders, heads |
| `ConvTranspose2d` | 18 | 9 in SPN, 4 + 4 in the two DPT decoders, 1 in the disparity head — all k=2, s=2, p=0 |
| `Linear` | 192 | ViT qkv/proj/fc1/fc2 |
| `LayerNorm` | 98 | ViT |
| `LayerScale` | 96 | ViT |
| `GroupNorm` | 8 | Gaussian decoder heads, 8 groups |
| `ReLU` | 54 | DPT decoder + heads |
| `GELU` | 48 | ViT MLP |
| `Dropout` / `Identity` | 146 / 305 | no-ops in eval |

Conv variants actually needed: 1×1 (stride 1, pad 0), 3×3 (stride 1, pad 1), 2×2
(transposed, stride 2), 16×16 (stride 16, the ViT stem), 5→128 k=2 s=2 (the skip
conv). GroupNorm has 8 groups; LayerNorm is over 1024 channels with eps 1e-6.

Attention: 16 heads × 64 dim, token count 577 (576 patches + CLS), batch 35 for
the patch encoder and 1 for the image encoder, scale 1/√64, no mask, no dropout
at inference. ggml needs a batched (35, 16, 577, 577) score matrix — 1.5 GB in
fp32 at 35×16×577² — so this is the one place where a fused/flash-style kernel
genuinely pays (spec C3 "process the encoder's patches in batches").

---

## 8. Checkpoint pickle structure (for the C1 unpickler)

The `.pt` is a zip (`torch.save`, `_use_new_zipfile_serialization`), format v1.0:

```
2572gikvuh_60000_lama/data.pkl              305,211 B  (protocol 2 pickle)
2572gikvuh_60000_lama/byteorder             b"little"
2572gikvuh_60000_lama/version               "3\n"
2572gikvuh_60000_lama/.data/serialization_id 40 B
2572gikvuh_60000_lama/data/0 .. data/1037   one raw blob per tensor
```

1,038 tensors, 1,038 blobs, **one storage per tensor, no sharing, no views**,
storage keys `"0".."1037"` in the order the record appears. Total 2,809,220,676 B
of tensor data (file 2,809,738,232 B; the rest is zip + pickle overhead).

### Opcodes present (counts)

| Opcode | n | Opcode | n |
| --- | --- | --- | --- |
| `PROTO` | 1 | `BINUNICODE` | 3,565 |
| `GLOBAL` | 3 | `BININT` | 302 |
| `STACK_GLOBAL` | 0 | `BININT1` | 4,713 |
| `REDUCE` | 2,078 | `BININT2` | 1,920 |
| `BINPERSID` | 1,038 | `LONG`/`LONG1` | 0 |
| `EMPTY_DICT` | 1,486 | `NEWFALSE` | 1,038 |
| `MARK` | 2,378 | `BUILD` | 1 |
| `TUPLE` / `TUPLE1-3` | 2,374 / 1,382 / 388 / 8 | `SETITEM` / `SETITEMS` | 1,486 / 4 |
| `BINPUT` / `LONG_BINPUT` | 256 / 11,028 | `BINGET` / `LONG_BINGET` | 5,187 / 1,484 |
| `EMPTY_TUPLE` | 1,040 | `STOP` | 1 |

No `BINFLOAT`, no `LONG`, no `EXT*`, no `FRAME`, no `MEMOIZE`, no `APPEND*`.

### GLOBAL / REDUCE targets (only three globals, all memoised)

| Global | Used as | Count |
| --- | --- | --- |
| `collections.OrderedDict` | `REDUCE` with `()` — the outer state dict, one per tensor's `backward_hooks`, and the `_metadata` dict | 1,040 constructions |
| `torch._utils._rebuild_tensor_v2` | `REDUCE` with a 6-tuple | 1,038 |
| `torch.FloatStorage` | only inside the `BINPERSID` tuple, never called | 1,038 persist-id references |

Per-tensor record (verified with a pickle VM over the real byte stream):

```
BINUNICODE  "<module>.<param>"
MARK
  MARK ('storage', torch.FloatStorage, "<key>", "cpu", <numel>) BINPERSID
  BININT  <storage_offset>            # always 0
  TUPLE   (<d0>,<d1>,...)             # size, C order
  TUPLE   (<s0>,<s1>,...)             # stride, contiguous row-major
  NEWFALSE                            # requires_grad
  BINGET OrderedDict → EMPTY_TUPLE → REDUCE   # backward_hooks = OrderedDict()
TUPLE
REDUCE   # _rebuild_tensor_v2
SETITEM  # into the state dict
```

After the last tensor: key `"_metadata"` → `OrderedDict()` with 1,486 entries
mapping module prefixes to `{"version": 1}`, then one `BUILD` and `STOP`. The
unpickler can skip `_metadata` entirely.

### Dtypes and layout

* Every tensor is `torch.float32` (`FloatStorage`), little-endian, storage offset
  0, `requires_grad=False`, C-contiguous (`stride[i] = prod(size[i+1:])`).
* Largest single tensor: `monodepth_model.monodepth_predictor.encoder.patch_encoder.head.weight`,
  shape (21841, 1024) = 22,365,184 elements = **89,460,736 bytes**. The same size
  appears once more for `image_encoder.head.weight`; both are dead (§2).
  Largest *live* tensor: any ViT `fc1.weight` (4096, 1024) = 16,777,216 B.
* Sum of all tensor bytes = 2,809,220,676; per-tensor iteration with a 96 MB
  buffer keeps C1 conversion peak memory around "largest tensor + constant", as
  the spec assumes (worst case 90 MB, not 2.8 GB).

C1 implementation is therefore small: protocol-2 unpickler handling
`PROTO GLOBAL EMPTY_TUPLE EMPTY_DICT MARK TUPLE TUPLE1-3 REDUCE BINPERSID
BINUNICODE BININT BININT1 BININT2 NEWFALSE BINPUT LONG_BINPUT BINGET
LONG_BINGET SETITEM SETITEMS BUILD STOP`, with `REDUCE` accepted only for
`OrderedDict()` and `_rebuild_tensor_v2`, `BINPERSID` only for
`('storage', FloatStorage, key, 'cpu', numel)`. Anything else → hard error.

---

## 9. Composer and unprojection: the Gaussian parameterisation

### Base values (initialiser, `models/initializer.py:127-278`) — no parameters

Measured shapes: `mean_x`, `mean_y`, `inv_z`, `scale` are 1×1×2×768×768 (layer
dim second), `quaternion` 1×4×1×1×1 (broadcast), `opacity` scalar, `colour`
1×3×2×768×768.

* grid: `x = 2*(0.5*stride + k*stride)/W - 1` for stride 2 (`:256-271`) → NDC in
  [-1,1], y down.
* `inv_z = max_pool2d(1/depth, 2, 2)` per depth layer (`surface_min`) → base
  disparity, clipped by the depth normalisation below (`:159-206`).
* depth normalisation: `depth *= 1/min(depth)` (min becomes 1.0), clamp to ≤100,
  `global_scale = 1/depth_factor` (`:148-150, 281-297`). Measured
  `global_scale = 3.039` for photo02 (min depth 3.04 m).
* base scale = `(1/inv_z) * (2*stride/W)`, i.e. the pixel footprint in metres at
  that depth (`:209-211, 274-278`). Then multiplied by `inv_z * z_predicted` in
  the composer (`models/composer.py:119-124`).
* base quaternion `(1,0,0,0)`, base opacity `min(1/layers, 0.5) = 0.5`, base
  colour = `avg_pool2d(image, 2, 2)` replicated to both layers, sRGB [0,1]
  (`:213-236`).

### Activations applied by the composer (`models/composer.py`)

Deltas arrive as 1×14×2×768×768 with channel order mean(3), scale(3),
quaternion(4), colour(3), opacity(1) (`models/heads.py:48-52`). `delta_factor`
(`models/params.py:55-66`): xy 1e-3, z 1e-3, colour 0.1, opacity 1, scale 1,
quaternion 1.

| Attribute | Formula | Notes |
| --- | --- | --- |
| mean x, y | `x = x_base + 1e-3*δx`; `y = y_base + 1e-3*δy`; final `X = z*x`, `Y = z*y` | NDC → ray (`composer.py:157-209`) |
| mean z | `inv_z = softplus(inverse_softplus(inv_z_base) + 1e-3*δz)`; `z = 1/(inv_z + 1e-3)` | inverse_softplus clamps at 1e-6 (`utils/math.py:62-67`) |
| scale | `sf = (max-min)*sigmoid(a*δs + b) + min`, `a = 10/9`, `b = logit(0.1) = -2.1972`, `min=0`, `max=10`; `σ = base_scale * sf` | `composer.py:21-28, 211-222`; `params.py:182-184` |
| quaternion | `q = q_base + 1*δq`, **not normalised here** | normalised later (`utils/linalg.py:22`) |
| colour | `c = sigmoid(inverse_sigmoid(clamp(base,0.01,0.99)) + 0.1*δc)`, then **sRGB→linear** | `composer.py:230-245`, `utils/color_space.py:31-58` |
| opacity | `α = sigmoid(inverse_sigmoid(0.5) + δα) = sigmoid(δα)` | base 0.5 for both layers |
| global | `mean *= global_scale`, `σ *= global_scale` | `composer.py:145-147` |

### Unprojection (`utils/gaussians.py:54-131`)

```
M = inv(ndc_matrix @ K_resized @ E),  E = I
ndc_matrix = [[2/W,0,-1,0],[0,2/H,-1,0],[0,0,1,0],[0,0,0,1]],  W=H=1536
K_resized = [[f_px*1536/w, 0, 768],[0, f_px*1536/h, 768],[0,0,1]]
=> M[:3,:3] = diag(w/(2 f_px), h/(2 f_px), 1), translation 0
```

Measured for photo02: `diag(0.6285, 0.3535, 1)` with a 4e-8 off-diagonal from
float32 inversion. Then

* means: `X = M[:3,:3] @ mean + t`
* covariance: `Σ = R(q) diag(σ²) R(q)ᵀ`, then `Σ' = M Σ Mᵀ`, then **fp64 SVD**,
  singular values sorted descending, `σ' = sqrt(sv)`, quaternion from the rotation
  matrix via **scipy** `Rotation.from_matrix` in fp64 (`utils/gaussians.py:134-169`,
  `utils/linalg.py:46-62`).

Two consequences for the port:

1. `M` is diagonal, so `Σ' = diag(sx,sy,1) Σ diag(sx,sy,1)` — a closed form. The
   only thing the SVD adds is *ordering* (descending singular values) and the
   corresponding axis permutation inside the quaternion. A port that skips the
   SVD must still emit the same ordering, or accept that quaternions and scales
   differ by a permutation that renders identically. This is 13% of the reference
   runtime, so skip it, but pin the ordering.
2. `q` and `-q` are the same rotation and scipy's sign choice is numerically
   unstable: the CPU and MPS runs of the *same* photo differ by `max|Δq| = 1.42`
   while the covariance matrices agree to 3e-3. **Never compare `.ply`
   quaternions elementwise.** Compare `R(q) diag(σ²) R(q)ᵀ`, or canonicalise the
   sign (e.g. force `w ≥ 0`).

### Coordinate frame

OpenCV: x right, y down, z forward (README; `composer.py:208` builds
`(z*x, z*y, z)` with y already pointing down from the NDC convention). Scene
centres around `(0,0,+z)` with z ≈ 1.5–5 m for a room, up to hundreds of metres
for sky/far background (photo02 reached z = 310 m after the depth clamp at 100
normalised units). No axis flip is applied anywhere in the reference — the
renderer consumes the same frame.

### PLY export (`utils/gaussians.py:346-482`)

Vertex: `x y z f_dc_0..2 opacity scale_0..2 rot_0..3` (14 floats, little-endian).
`opacity` and `scale_*` are stored as **logits** (`log(σ)`, `logit(α)`),
`f_dc` as SH DC of the **sRGB** colour. Extra elements: `intrinsic` (9 floats,
original-image K with `f_px`, `w/2`, `h/2`), `extrinsic` (16, identity),
`image_size` (2 uint32), `frame` (2 int32: 1 frame, N points), `disparity`
(0.1/0.9 quantiles), `color_space` (1 uchar = 0, sRGB), `version` (3 uchar
1.5.0). For C5 `.psplat` this is the mapping target: 14 values per Gaussian, and
note the logit/log encodings — the spec's attribute list already matches.

Quirk worth knowing: `opacity_logit = log(α/(1-α))` in fp32, so any Gaussian whose
sigmoid saturates to exactly 1.0 gets **`+inf`** in the PLY (36 of 1,179,648 in
photo01, 0 in the other two). C5 must clamp `α` to `(ε, 1-ε)` before taking the
logit, or it will emit inf/NaN for bright saturated splats.

---

## 10. Golden tensors

Location `$SPLAT_DATA_DIR/golden/` (outside the repo, 5.9 GB):

| Photo | Source | Size | Gaussians | Forward (CPU) |
| --- | --- | --- | --- | --- |
| `photo01_living_room` | Wikimedia CC0, rural living room, 1280×873 | 2.0 GB | 1,179,648 | 100.5 s |
| `photo02_street` | Wikimedia CC0, street in Ishkashim, 1280×720 | 2.0 GB | 1,179,648 | 87.3 s |
| `photo03_reading_room` | Wikimedia CC0, British Museum Reading Room, 1280×964 | 2.0 GB | 1,179,648 | 66.1 s |

All three have no EXIF focal length, so they exercise the 30 mm fallback. Sources,
authors and licence URLs are in `golden/SOURCES.md`; per-photo run data in
`golden/<id>/000_meta.json` and `golden/RUN.json`. `tools/golden/dump_golden.py`
regenerates them; `tools/golden/README.md` explains the layout and how to compare.

Stage files per folder: preprocess (010/011), encoder in/out (020/021_00..04),
depth decoder (030), disparity head (031), depth alignment (040, identity proof),
monodepth (041), Gaussian decoder in (050) and base values (051_*), decoder out
(060_texture/geometry), delta values (070), composer (080_*), unprojected metric
Gaussians (090_*) and `reference.ply`.

**Device matters more than expected.** CPU fp32 is bit-reproducible (two runs
byte-identical), so the golden set is CPU. MPS vs CPU for the same photo differs
by up to:

| tensor | max abs | max rel (|x|>1e-3) |
| --- | --- | --- |
| encoder level 0 / 2 | 5.3e-7 / 2.4e-6 | 3.1e-4 / 1.6e-3 |
| depth decoder output | 5.0e-6 | 1.8e-3 |
| monodepth | 40.3 m | 5.4e-3 (on far tails) |
| delta values | 0.507 | 8.7 (pre-activation, then scaled by 1e-3…0.1) |
| composer mean | 0.083 m | 0.45 |
| composer opacity | 6.5e-3 | 1.8e-2 |

So the spec's "1e-3 max relative error per stage" is already violated by moving
the *same* PyTorch model between two backends, at individual elements. Parity
must be judged against the CPU golden with a percentile + max-abs criterion (or
on post-activation attributes and rendered images), not a raw per-element 1e-3
rel across all 1.18 M Gaussians. Worth pinning down in the testing section when
`tools/parity` is written.

---

## 11. Porting notes that fall out of this

1. **Only 655.5 M live params.** Drop the two ViT classifier heads and the whole
   `depth_alignment` subtree: 2.44 GiB fp32 → 1.22 GiB fp16 for the model.
2. **The encoder dominates** (59% of runtime, ~96% of the live params, 35-patch
   ViT-L batch). Batch the 35 patches, reuse the KV-free path, and consider fp16
   only for the conv/linear weights with fp32 accumulation.
3. **The 1536² intermediates are the memory problem**, not the weights: encoder
   level 0 (604 MB) and the DPT decoder output (604 MB) are each bigger than all
   the Gaussian parameters together. A fused "decode then head" pass that never
   materialises the full 768² features is the difference between 3 GiB and 10 GiB.
4. **Prep: no padding, anisotropic 1536² bilinear resize with `align_corners=True`.**
   Do not letterbox, do not use `align_corners=False` — it changes the input.
5. **`nn.Linear` transposes its weight on every call** (192 `aten::t` per run).
   ggml wants weights pre-transposed at conversion time.
6. **GroupNorm with 8 groups** on 768²×128 tensors: 8 reductions over 8×128×768²
   elements; a naive per-group two-pass will be slow. LayerNorm (1024) is only in
   the ViT.
7. **Attention is 577×577×16×35** — 1.5 GB of scores in fp32 if materialised.
   Either flash-style tiling or fp16 scores.
8. **Skip the fp64 SVD** in the unprojection (13% of runtime, closed form exists)
   but reproduce the descending-σ ordering, and do not use quaternions as a
   parity target.
9. Colour pipeline: sRGB in → linearRGB Gaussians → sRGB on PLY export. If the
   `.psplat` stores linear (as the composer produces), the renderer must not
   apply the PLY's sRGB convention twice.
10. The 30 mm focal default is a *scale* assumption: a wrong EXIF focal shifts
    metric scale (and thus comfort/eye separation). C2 should surface the
    estimated `f_px` to the UI, as the spec already hints.
11. Clamp opacity before taking the logit for `.psplat`/`.ply` (the reference
    writes `+inf` when α saturates to 1.0 — 36 splats in photo01).

---

## 12. Reproduce

```bash
# reference
DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
git clone --depth 1 https://github.com/apple-aiml-research/ml-sharp \
    "$DATA"/ref/ml-sharp
python3.12 -m venv "$DATA"/ref/venv-sharp
"$DATA"/ref/venv-sharp/bin/pip install torch==2.8.0 torchvision==0.23.0
"$DATA"/ref/venv-sharp/bin/pip install timm==1.0.20 pillow==11.3.0 \
    pillow-heif==1.1.1 plyfile==1.1.2 click==8.3.0 imageio==2.37.0 numpy==2.3.3 \
    scipy==1.16.2 matplotlib==3.10.6
"$DATA"/ref/venv-sharp/bin/pip install -e \
    "$DATA"/ref/ml-sharp --no-deps   # no gsplat: rendering only

# checkpoint
curl -L -O https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt
shasum -a 256 sharp_2572gikvuh.pt   # 94211a75…

# golden tensors (CPU, ~90 s and ~9 GiB per photo)
cd <repo>
DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
"$DATA"/ref/venv-sharp/bin/python tools/golden/dump_golden.py \
    --checkpoint "$DATA"/weights/sharp_2572gikvuh.pt \
    --out "$DATA"/golden \
    --photos "$DATA"/golden/inputs/*.jpg \
    --device cpu
```

Scratch scripts used for the numbers above (op trace, pickle VM, benchmark,
per-stage timing) are in `$SPLAT_DATA_DIR/runs/`.
