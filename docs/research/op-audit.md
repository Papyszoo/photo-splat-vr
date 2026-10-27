# SHARP op audit on ggml-vulkan, reuse scan, port plan

Task W1-opaudit. Measured on this host unless marked. Target stack: ggml
`a05c9922` / v0.27.0 (repo submodule), Vulkan backend through MoltenVK 1.4.363
on an Apple M4 (10-core, 16 GiB UMA), macOS arm64.

Reproduce: `runs/opaudit/` (build dir, harness, logs). Build:
`cmake -S third_party/ggml -B runs/opaudit/build -G Ninja -DGGML_VULKAN=ON
-DGGML_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release`, then
`cmake --build ... --target test-backend-ops`. Harness:
`runs/opaudit/harness/sharp_ops.cpp` (exact SHARP shapes),
`conv_probe*.cpp`, `mm_probe.cpp` (CPU/Vulkan vs fp64). Logs in the same dir.
This source should move to `tools/` before the repo is published; it is
deliberately outside the tree for now.

MoltenVK limits seen: `maxStorageBufferRange` = 4 GiB, `maxBufferSize` = 9.5 GiB,
shared memory 32 KiB, subgroup 32, no cooperative matrix, fp16 yes, bf16 no.
A 1.2 GB tensor (128x1536x1536 f32) allocates and computes.

---

## 1. Op map and Vulkan support

### 1.1 ATen -> ggml

Every op in the inventory (`docs/research/sharp-reference.md` §7) maps as follows.
"Vulkan" is `ggml_backend_vk_device_supports_op` (v0.27.0) plus a run at SHARP
shape; line numbers are `third_party/ggml/src/ggml-vulkan/ggml-vulkan.cpp`.

| ATen (count) | ggml op | Vulkan | Notes |
| --- | --- | --- | --- |
| `addmm`/`t` (192) | `MUL_MAT` (+`ADD` bias) | yes | weights pre-transposed at conversion; F32 and F16 weights both fine |
| `convolution` Conv2d (84) | `CONV_2D` (`ggml_conv_2d_direct`) | yes, F32 in / F32 out, F16 or F32 weights; **not** CWHN layout (`:16049`) | keep WHCN; `ggml_conv_2d` is *not* this op, see §1.4 |
| ConvTranspose2d (18) | `CONV_TRANSPOSE_2D` (`ggml_conv_transpose_2d_p0`) | yes (`:16049`) | kernel `[KW,KH,Cout,Cin]`, k=2 s=2 |
| `native_layer_norm` (98) | `NORM` (+`MUL`/`ADD` affine) | yes, F32 only (`:15762`) | eps 1e-6 |
| `native_group_norm` (8) | `GROUP_NORM` (+`MUL`/`ADD`) | yes, F32, contiguous (`:15760`) | 8 groups on 128 ch |
| `_scaled_dot_product_flash_attention_for_cpu` (48) | `FLASH_ATTN_EXT` | yes (`:15570`); needs head dim %8, Q F32, out F32 | 64/64 OK; K/V F16 or F32 |
| `gelu` (48) | `UNARY/GELU_ERF` | yes | exact erf, not tanh |
| `relu`/`relu_` (54) | `UNARY/RELU` | yes | |
| `sigmoid`/`softplus`/`log`/`neg`/`abs`/`sqrt` | unary or `SQRT`/`SQR` | yes | |
| `add/mul/sub/div/rsub` (289) | `ADD/MUL/SUB/DIV` (+`SCALE`) | yes | broadcast OK |
| `pow` (1) | `SQR` | yes | square only |
| `clamp` (3) | `CLAMP` | yes | |
| `upsample_bilinear2d` (2 + preprocess) | `UPSCALE`/`INTERPOLATE` | yes, F32 only (`:15823`) | `GGML_SCALE_FLAG_ALIGN_CORNERS` implemented correctly (`:10526`) |
| `max_pool2d_with_indices` (2) | `POOL_2D` MAX | yes (`:15945`) | indices unused |
| `avg_pool2d` (1) | `POOL_2D` AVG | yes | |
| `cat` (32) | `CONCAT` | yes | |
| `slice/view/permute/transpose/unsqueeze/expand/repeat/select/unbind/split` (1500+) | `VIEW/PERMUTE/RESHAPE/CONT/REPEAT` | yes | reshapes are free, `CONT` materialises |
| `t` in `nn.Linear` (192) | — | — | fold into GGUF (store transposed) |
| `where`+`le`+`bitwise_not` (4) | **none** | — | compose as `lo + (hi-lo)*STEP(x-t)` (STEP is supported) |
| `min.dim` (1) | **none** | — | no MIN reduce in ggml; scalar, fold into the composer/host |
| `arange`/`ones_like`/`fill_`/`scalar_tensor` | `ARANGE/FILL` | yes | |
| `clone`/`empty`/`lift_fresh` | `CONT/DUP` | yes | |
| `_local_scalar_dense` (.item) | — | — | bake the constant at graph-build time |
| SVD/eigh (post-network) | **none** | — | closed form, see §3.3 |

Net: every SHARP op that has a ggml equivalent is supported on ggml-vulkan
v0.27.0. Three things have no ggml op at all (`where`, global `min`, SVD), and
one has no efficient op (trimmed patch merge); all four have cheap workarounds
(§3.3).

### 1.2 Exact-shape runs (MoltenVK)

Medians of 3, parity vs ggml-cpu on the same graph. `n/a` = CPU reference too
expensive at that size; the case still ran, output finite.

| Case (SHARP shape) | ms | max abs | NMSE | cos |
| --- | ---: | ---: | ---: | ---: |
| conv 16x16 s16, 3->1024, 35x384x384 (patch embed) | 92.6 | 1.6e-5 | 2.7e-13 | 1.000000 |
| same, F16 weights | 93.9 | 1.2e-2 | 6.7e-7 | 1.000000 |
| conv 3x3 1024ch @96² (SPN tap projection) | 527 | 2.2e-4 | 3.2e-12 | 1.000000 |
| conv 3x3 256ch @768² (stress; real DPT is 384²) | 2016 | n/a | n/a | n/a |
| conv 3x3 128ch @768² (Gaussian head) | 494 | n/a | n/a | n/a |
| conv 1x1 2048->1024 @48² | 30 | 4.0e-5 | 7.1e-13 | 1.000000 |
| conv 2x2 s2 5->128 @1536² (skip conv) | 17.8 | n/a | n/a | n/a |
| tconv 2x2 1024ch 48²->96² | 226 | 2.1e-5 | 3.6e-13 | 1.000000 |
| tconv 2x2 256ch 192²->384² (parity proxy) | 222 | 5.7e-6 | 9.1e-14 | 1.000000 |
| tconv 2x2 256ch 384²->768² | 891 | n/a | n/a | n/a |
| tconv 2x2 128ch 768²->1536² (disparity head) | 877 | n/a | n/a | n/a |
| layernorm 1024 over 35x577 tokens | 3.5 | 4.8e-7 | 3.6e-15 | 1.000000 |
| GELU_ERF 4096x20195 | 8.0 | 1.2e-7 | 1.9e-14 | 1.000000 |
| mul_mat 1024x3072, n=20195, F32 weights | 93.6 | 3.9e-3 | 6.8e-8 | 1.000000 |
| mul_mat 1024x3072, n=20195, F16 weights | 44.4 | 0.20 | 2.3e-5 | 0.999989 |
| mul_mat 1024x1024, n=20195, F16 weights | 15.6 | 0.20 | 2.3e-5 | 0.999989 |
| flash attn 64/64, 16 heads, 577 tok, 35 batch, F16 KV | 64.5 | 1.1e-4 | 1.3e-6 | 0.999999 |
| same, F32 KV | 118 | 1.2e-4 | 1.3e-6 | 0.999999 |
| softmax 577x577x16 | 1.0 | 9.3e-10 | 8.8e-15 | 1.000000 |
| group_norm 8 groups, 128ch @768² | 20.1 | n/a | n/a | n/a |
| group_norm 8 groups @96² | 0.7 | 1.2e-7 | 3.6e-15 | 1.000000 |
| max pool 2x2 @1536²x2 | 0.5 | 0 | 0 | 1.000000 |
| avg pool 2x2 @1536²x3 | 0.6 | 0 | 0 | 1.000000 |
| add broadcast 1024x577x35 + 1024 | 4.0 | 0 | 0 | 1.000000 |
| composer chain (add/scale/sigmoid/clamp/log/softplus/sqrt/abs) 14x2x768² | 12.4 | 1.8e-7 | 4.6e-15 | 1.000000 |
| interp 1536->768, align_corners=False | 0.7 | 3.0e-8 | 1.9e-15 | 1.000000 |
| interp 1536->384, align_corners=False | 0.5 | 3.0e-8 | 1.9e-15 | 1.000000 |
| interp 1280x873->1536, **align_corners=True** | 2.1 | 1.3e-4 | 2.3e-9 | 1.000000 |
| **for contrast:** `ggml_conv_2d` patch embed (F16 im2col) | 14.0 | 0.127 | 1.7e-5 | 0.999991 |
| **F32 im2col + mul_mat** patch embed | 27.0 | 3.4e-3 | 6.8e-8 | 1.000000 |

31/31 cases OK, 0 unsupported, 0 failing. Stock suite confirms the same picture:

| Suite | Result |
| --- | --- |
| `test-backend-ops support -b Vulkan0` (all cases) | 18,016 supported / 4,087 not; spot-checked rejects are bf16, f16 **input** conv, CWHN conv, quantised weights, 4D-batched/permuted matmul, and f16 copy/out-prod variants — none are SHARP ops |
| NORM, GROUP_NORM, UPSCALE, INTERPOLATE, POOL_2D, SOFT_MAX, GELU_ERF, SIGMOID, SOFTPLUS, CONCAT, PAD, SUM, SCALE, CLAMP, LOG, EXP | 735/735 passed |
| MUL_MAT F32 | 258/258 passed |
| CONV_2D + CONV_TRANSPOSE_2D, F32 kernels | 981/981 passed; the 589 skipped cases are all CWHN layout |
| FLASH_ATTN_EXT hsk=hsv=64, kv=577/1024 | 440/440 passed (11 not-supported cases are head dims 65/67, i.e. not %8) |
| CONV_2D | supported for every F32 and F16-kernel case in WHCN layout; **never** for CWHN |

### 1.3 Vulkan constraint table (what the graph must respect)

| Constraint | Consequence for SHARP |
| --- | --- |
| `CONV_2D`: input F32, out F32, weights F32/F16, WHCN contiguous (`:16049`) | fp16 **weights** only; activations stay f32 |
| `CONV_2D` unsupported for channel-contiguous (CWHN) layout | keep WHCN for conv graphs; CWHN only for 1x1/mul_mat views |
| `NORM`, `GROUP_NORM`, `UPSCALE`, `POOL_2D`: F32 only | those stages cannot run in f16 activations |
| `FLASH_ATTN_EXT`: Q F32, out F32, head dim %8, K/V F32/F16/bf16/quant (`:15570`) | Q f32, K/V f16, head 64 OK, 35 patches in `ne[3]` in one call |
| `MUL_MAT`: F16/F32 weights, F32 or F16 activations | f16 weights give 2.1x on qkv with cos 0.999989 |
| `UPSCALE`: F32 only; align_corners implemented as `(out-1)/(in-1)`, offset 0 (`:10526`) | preprocessing resize can run on GPU and is exact vs PyTorch (NMSE 2.3e-9) |
| Max buffer 4 GiB (MoltenVK), 1.2 GB tensors tested OK | head TConv at 1536² fits; still worth fusing for memory |
| `GGML_VK_MAX_NODES_PER_SUBMIT` default 100 | split the ~260-node forward into several submits to avoid the RADV device-lost risk |

### 1.4 Two traps found

1. **`ggml_conv_2d` is not `CONV_2D`.** It builds `IM2COL` with `dst_type = F16`
   for every non-BF16 kernel (`ggml.c:4785`) + `MUL_MAT`. At ViT scale (patch
   embed, 35 patches) the three options measure:

   | Path | ms | max abs | NMSE | cos |
   | --- | ---: | ---: | ---: | ---: |
   | `ggml_conv_2d` (F16 im2col) | 14 | 0.127 | 1.7e-5 | 0.999991 |
   | own `IM2COL(F32)` + `MUL_MAT` | 27 | 3.4e-3 | 6.8e-8 | 1.000000 |
   | `ggml_conv_2d_direct` (native) | 93 | 1.6e-5 | 2.7e-13 | 1.000000 |

   So the exact-and-fast choice is **F32 im2col + matmul** for the 3-channel
   patch embed; native direct conv is the exact choice for 3x3 layers where an
   im2col buffer would be 9x the tensor (5.4 GB at 768², so never im2col there).
   Never use `ggml_conv_2d` itself for a parity-gated stage.
2. **The probe caught a 4% error that turned out to be the F16 im2col**, not the
   Vulkan conv. The native `CONV_2D` matches an fp64 host reference to 4e-13
   NMSE on both CPU and Vulkan (`conv_probe_direct.cpp`).

### 1.5 Where MoltenVK may differ from RADV (PS5)

| Area | MoltenVK (measured) | RADV / RDNA2 (expected) | Action |
| --- | --- | --- | --- |
| cooperative matrix | absent (`matrix cores: none`) -> scalar f32 conv/attn paths | RADV may expose `VK_KHR_cooperative_matrix`; conv then takes the **cm1 path with f16 accumulators** (16x16x16 f16-acc is hardcoded) | re-run this audit on the console; for parity gates set `GGML_VK_DISABLE_COOPMAT=1` and compare |
| subgroup size | 32 | 64 (wave64) | conv tile selection and flash-attn perf differ; numerics of f32 path unchanged |
| shared memory | 32 KiB | 64 KiB | conv BS_CRS selection differs; more occupancy on PS5 |
| max buffer | 4 GiB storage range, 9.5 GiB buffer | same class (11.44 GiB heap) | 1.2 GB peak tensor OK, but keep head fusion on the list |
| fp16/bf16 | fp16 yes, bf16 no | fp16 yes, bf16 yes | we do not use bf16 |
| driver quirks | MoltenVK is not a parity target per spec | target | all numeric gates must be repeated on RADV before Spike A is called done |

Timing on MoltenVK (a 5-10x slower GPU stack than the console is expected): the
35-patch ViT at 24 blocks is roughly `24 x (44 + 64 + 16 + 59 + 59) ms ~ 5.8 s`,
the two decoders ~4-5 s, total ~12-15 s. Extrapolated to RDNA2 this is inside the
60 s budget; measure, do not assume.

---

## 2. Reuse scan

| Source | License | What it actually is | Covers | Verdict |
| --- | --- | --- | --- | --- |
| **llama.cpp `tools/mtmd/clip.cpp`** (6,755 lines) | **MIT** | ViT inference in ggml: patch-embed conv, CLS/pos-embed with `ggml_interpolate`, `ggml_norm` + affine, `GELU_ERF`, `ggml_flash_attn_ext`, q/k/v `VIEW`/`PERMUTE` plumbing, GGUF loading | **SHARP §6.1 encoder** (block structure, attention layout, f16 weight policy); C4 ViT-S block | **Adapt** (copy block/attention graph shape, not the GGUF/CLIP plumbing). Best single reference. Caveat: its patch embed uses `ggml_conv_2d` (F16 im2col), swap to F32 im2col |
| **Acly/vision.cpp** (`src/visp/{nn,dino,depth-anything}.cpp`) | **MIT** | DINOv2 ViT + DPT neck/head in ggml, Depth Anything V2 end to end, CPU and **Vulkan** backends; F16 GGUF models on HF | **SHARP §6.2/§6.4 DPT decoders** (feature fusion/residual/1x1/TConv), **C4 full graph**, CWHN vs WHCN conv policy | **Adapt** — cleanest C4 blueprint; also documents `conv_2d_direct` vs im2col trade-off |
| **localai-org/depth-anything.cpp** | **MIT** | DA V2 + DA V3 in C++17/ggml, ViT/DPT/attention split into modules, DA2->GGUF converter scripts, host-side `gs_adapter` (depth+camera -> 3D Gaussians), Vulkan backend | C4 graph, C4 conversion, Gaussian assembly reference (§composer), attention/flash patterns | **Adapt** (C4) / **follow** (`gs_adapter` math, DA3-specific) |
| **Acly/Depth-Anything-V2-GGUF** (HF) | weights Apache-2.0 (Small) | pre-converted F16/F32 GGUF of DA V2 Small/Base/Large | C4 weights, shortcut only | **Follow** — do not depend on it; convert the Apache-2.0 safetensors ourselves (single fetch path, spec C4) |
| **localai-org/free-splatter.cpp** | **Apache-2.0** | ggml feed-forward 3DGS (FreeSplatter): predicted Gaussian attributes, `.splat` encoder, 3x3 cyclic-Jacobi eigensolver/SVD in `linalg.h`, OpenCV->OpenGL remap | **SHARP §6.5/§9** Gaussian parameter assembly analogy; **unprojection** (3x3 SVD closed form); .psplat/.splat packing | **Follow** / adapt `linalg.h` (~213 lines) if a host SVD is wanted |
| **vkgs** | **MIT** | Vulkan 3DGS renderer: frustum cull, radix sort, instanced quads | **C6 renderer** (spec already mandates it) | **Vendor** (C6, not this task) |
| kutipense/Depth-Anything-V2-ggml | none | unlicensed DA V2 ggml experiment | — | **Do not use** |
| DracoUnion/ggml-sharp | none | C# reimplementation of ggml-cpu; unrelated to SHARP despite the name | — | **Ignore** |
| Depth Pro ggml port | — | **none found** (searched GitHub, 2026-10-25) | — | write from spec + reference code |
| ggml/Vulkan 3DGS rasteriser | — | **none found**; vkgs is not ggml | — | C6 stays vkgs |

Licence compatibility: MIT and Apache-2.0 are both compatible with
GPL-3.0-or-later. Copying
any of these into the tree requires attribution in `THIRD_PARTY_NOTICES.md` and,
for Apache-2.0, the NOTICE/change statement. SHARP's own `LICENSE`/`LICENSE_MODEL`
still forbid copying its code or shipping its weights — the reuse above is
upstream code only.

---

## 3. Port plan

### 3.1 Per-stage graph and dtype

Notation: activations are F32 (Vulkan conv/norm/pool require it), weights F16
unless noted. `B` = patch batch; all shapes at 1536x1536.

| Stage (golden file) | Graph | Dtype | Peak activations | Gate |
| --- | --- | --- | --- | --- |
| 0 preprocess (`010/011`) | CPU: decode, EXIF orient, bilinear `align_corners=True` to 1536², `2x-1`. GPU option: `INTERPOLATE` + `ALIGN_CORNERS` flag + `SCALE/ADD` (verified 2.3e-9 NMSE) | f32 | 28 MB | max abs <= 1e-5 vs 011 |
| 1a patch embed (`021_00`) | `IM2COL(F32)` + `MUL_MAT` (27 ms; `ggml_conv_2d` is F16-staged, direct conv 93 ms) | F32 im2col, weights F32 (measured) or F16 | 62 MB im2col | cos >= 0.9999, p99 rel <= 1e-3 |
| 1b ViT blocks x24 (`021_01..02`) | per block: `NORM`+affine -> qkv `MUL_MAT` -> `RESHAPE` -> `FLASH_ATTN_EXT` (Q f32, K/V f16) -> `MUL_MAT` proj -> `ADD` -> `NORM` -> fc1 `GELU_ERF` fc2 -> `ADD`; LayerScale as `MUL` with 1024-vector | f16 W, f32 act, f16 K/V | 83 MB tokens + 331 MB MLP (76 MB at B=8) | per-level cos >= 0.999, p99 rel <= 1e-3 |
| 1c merge + SPN upsamplers (`021_03..04`) | trimmed 24x24-tile gather (5x5 and 3x3, trim 3/6 px) -> `CONT` -> 1x1 `MUL_MAT` -> `CONV_TRANSPOSE_2D`; image encoder on the 384² level; `CONCAT` + 1x1 `fuse_lowres` | f16 W, f32 act | 604 MB (level 0) + 151 MB | cos >= 0.999 |
| 2 depth DPT + head (`030/031/041`) | 4x 3x3 `CONV_2D` (256ch) -> `FeatureFusionBlock` (relu/3x3 x2 residual, TConv x2, 1x1); head 3x3 256->128, TConv 128@768²->1536², 3x3 128->32, 1x1 32->2, relu; `monodepth = f / clamp(disp)` | f16 W, f32 act | 604 MB + **1.2 GB** (head TConv) | cos >= 0.9999 on 031; 041 p99 rel <= 1e-3 |
| 3 alignment (`040`) | identity, skip entirely | — | — | assert 1.0 map |
| 4 Gaussian decoder (`050/051/060/070`) | initialiser (host/`POOL_2D`, `ARANGE`, `FILL`, `SOFTPLUS`); skip conv 5->128 s2; DPT 128ch with skip fusion; 2x (group_norm 8g, relu, 3x3 128->64->128, 1x1->32); `prediction_head` 1x1 32->6 and 32->22 -> cat 14ch | f16 W, f32 act | 302 MB x2 + 76 MB | cos >= 0.999 per tensor |
| 5 composer (`080`) | `POOL_2D` max/avg, elementwise (sigmoid/log/softplus/clamp/pow), `STEP`-based `where`, global min via host; output directly into the `.psplat` SoA buffer | f32 | 66 MB | p99 rel <= 1e-3, no NaN |
| 6 unproject (`090`) | host CPU: `M = diag(w/2f, h/2f, 1)`, closed-form `Sigma' = M Sigma M^T`, sort sigma desc, quaternion from rotation (canonical `w>=0`), clamp opacity before logit | f32/f64 | — | compare covariances, **never** raw quaternions |

No stage needs a custom Vulkan shader for correctness. The two candidates for
speed/memory are optional: a fused trimmed-tile merge (one pass instead of two
GET_ROWS passes, ~1.2 GB traffic saved per level), and a fused
TConv+3x3+1x1 disparity head (removes the 1.2 GB tensor).

### 3.2 Activation memory at 1536²

| Live set | Size (f32) |
| --- | ---: |
| input + pyramid (1536², 768², 384² x3ch) | 37 MB |
| 35 patches 3x384² | 62 MB |
| ViT tokens 35x577x1024 | 83 MB |
| MLP hidden 35x577x4096 | 331 MB (76 MB at B=8) |
| K/V f16 35x16x577x64 | 83 MB |
| encoder levels 256@768², 256@384², 512@192², 1024@96², 1024@48² | 878 MB |
| decoder output 256@768² + one fusion buffer | 1.21 GB |
| disparity head 128@768² + 128@1536² + 32@1536² | 1.83 GB (0.32 GB fused) |
| Gaussian decoder 128@768² x2 + 32@768² x2 + deltas | 0.82 GB |
| **sum, worst case without liveness reuse** | **~5.3 GB** |
| **with ggml-alloc liveness + B=8 + fused head** | **~2.5-3 GB** |
| weights 655.5M f16 | 1.31 GB |

So 8 GiB is reachable with patch batching and liveness; the reference's 5.6 GiB
MPS peak is consistent. f32 weights (2.62 GB) also fit if f16 parity fails.

### 3.3 Missing pieces and workarounds

| Need | Workaround | Custom op? |
| --- | --- | --- |
| `where` (sRGB->linear) | `lo + (hi-lo)*STEP(x-t)`; 2 extra 7 MB passes | no |
| global `min(depth)` (initialiser) | scalar; read back the 2x1536² map or compute at upload | no (host) |
| trimmed patch merge | `GET_ROWS` twice with precomputed trim indices (supported, exact); or one custom gather shader | optional |
| 3x3 SVD in unproject | closed form (M diagonal) + descending-sigma sort; ~10 ms host | no (host) |
| `softplus^-1` | `LOG(EXPM1(x))` with clamp at 1e-6 (EXPM1 supported) | no |
| GroupNorm affine | `GROUP_NORM` then per-channel `MUL`/`ADD` | no |

If a custom op is wanted later, only one has a real payoff: `sharp_merge(tiles,
steps, trim)` — a compute shader that gathers the `steps x steps` 24x24 token
tiles into the 96x96 (or 48x48) feature map in one pass, replacing two
`GET_ROWS` passes per level (~1.2 GB of traffic at level 0). Note that
`ggml_map_custom*`/`GGML_OP_CUSTOM` is **not** dispatched to Vulkan (it falls
through `supports_op` to false and runs on CPU), so a Vulkan version means a
small backend patch: `GGML_OP_SHARP_MERGE` with op params `{steps, trim}`, a
`sharp_merge.comp` shader, and a `patches/` entry. Same for the second,
memory-only candidate: a fused `TConv 128@768² + 3x3 + 1x1` disparity head that
removes the 1.2 GB intermediate. Neither is on the critical path for a first
working port.

fp16 policy, in one line: **weights f16, activations f32, attention K/V f16**.
The ViT is the only place fp16 buys real time (2.1x on qkv); conv, norm, pool
and interpolate activations cannot be f16 on this backend, and the f16-weight
error (cos 0.999989 on qkv, 0.999999 on attention) is inside the spec's fp16
gate but must be re-checked end to end.

### 3.4 Implementation order (each step gated before the next)

1. C1 converter + F16-weight GGUF; check tensor round-trip bit-exact.
2. Preprocess + patch embed + one ViT block; gate on a dumped block-0 output
   (add a temporary hook to `tools/golden`), then all 24 blocks.
3. Merge/upsamplers + image encoder; gate on `021_00..04` (5 levels).
4. Depth DPT + head; gate on `030/031`, then `041` monodepth.
5. Gaussian decoder; gate `051_*`, `060_*`, `070`.
6. Composer + unproject; gate `080_*`, `090_*` (covariance), then
   `reference.ply` render PSNR >= 35 dB from the 8 fixed poses.
7. Repeat 3-6 on RADV; then console timing.

Gate definition: fp32 CPU ggml must match the CPU golden with p99 relative error
<= 1e-3 **and** max abs within the per-tensor tolerance recorded in
`golden/<id>/000_meta.json`; fp16 runs need cosine >= 0.999 per tensor. The
golden doc already shows that a raw per-element 1e-3 max-rel is violated even
between CPU and MPS PyTorch, so do not gate on it. Also pin quaternion sign and
compare covariances.

### 3.5 C4: Depth Anything V2 Small

| Item | Value |
| --- | --- |
| Weights URL | `https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf/resolve/main/model.safetensors` |
| Size / SHA-256 | 99,173,660 B / `3152477ce0d8d6978d76b995120de97cb5b928701fd0f817769f59e249a16b70` |
| Alternative | `depth-anything/Depth-Anything-V2-Small` `depth_anything_v2_vits.pth`, 99,218,434 B (pickle; avoid) |
| Licence | **Apache-2.0** (model card and repo); Small is the only V2 size that is not CC-BY-NC |
| Format | safetensors, F32 only, 287 tensors, 33,296 B header; parse the header + raw blobs directly, no torch/pickle needed |
| Params | 24.79 M; F16 GGUF ~50 MB |
| Config | DINOv2 ViT-S/14: 384 dim, 6 heads, 12 blocks, 518 input (short side, multiple of 14), ImageNet mean/std; DPT fusion 64, head 32; output 1 ch **relative inverse depth** after ReLU |

Graph (verified against the safetensors shapes and the HF head definition):

```
resize short side 518 (bicubic, keep aspect, ensure mult 14) -> ImageNet normalise
patch_embed conv 14x14 s14 (384/3/14/14) -> 37x37=1369 tokens + CLS -> 1370
12 x [NORM, qkv, FLASH_ATTN_EXT (6 heads, 64 dim), proj, LayerScale,
      NORM, fc1 384->1536, GELU, fc2, LayerScale]
taps at blocks 3,6,9,12 -> NORM each -> reassemble 1x1 to 48/96/192/384
   (+ TConv k4s4 / k2s2 / none / 3x3 s1 for the 4/2/1/0.5 factors)
-> 3x3 conv to 64 -> 4 fusion blocks (residual 3x3 x2, bilinear x2
   align_corners=True, 1x1 projection)
-> head: 3x3 64->32, bilinear x2 align_corners=True, 3x3 32->32, relu,
   1x1 32->1, relu -> relative disparity
```

Post: `depth_m = assumed_scale / disparity` (same form as SHARP's monodepth);
one Gaussian per grid pixel (stride 2 at short side ~1036-1218 gives 0.35-0.5 M),
scale from pixel footprint at depth, colour from the pixel, opacity reduced or
dropped where the local depth gradient exceeds a threshold. Runs in f16 weights /
f32 activations. It reuses the same ViT block as C3, so implement C4 after the
SHARP encoder block is gated; only the DPT/head part is new.

Memory/time: weights 50 MB, activations < 200 MB, ~60 GFLOP at 518 short side, so
the 5 s budget is not at risk.

---

## 4. Bottom line

- **0 unsupported and 0 failing ops** for SHARP's inventory on ggml-vulkan
  v0.27.0 at the shapes SHARP needs (31/31 harness cases, 735+981+258+440 stock
  tests). Four non-ops are solved by composition or a host step.
- **Top reuse: llama.cpp `tools/mtmd/clip.cpp`, MIT** (ViT block + flash-attn
  layout); for C4, Acly/vision.cpp (MIT) is the closest end-to-end blueprint.
- **Main blockers are numeric/performance, not API:** (1) `ggml_conv_2d`'s F16
  im2col must be avoided or the 1e-3 parity gate fails; (2) RADV's cooperative
  matrix path uses f16 accumulators and must be disabled or explicitly accepted;
  (3) the 1.2 GB disparity-head tensor and the trimmed merge want fusion; (4) all
  numbers here are MoltenVK and must be repeated on RADV/PS5.

---

## 5. Console result (2026-10-26, RADV on PS5, title route)

The harness ran as the homebrew title `PPSA99201` on firmware 13.60
(`runs/console_session_2/03_bench_op_audit.console.log`): 35/35 cases pass,
`pass driver-default: ok=35 fail=0 unsupported=0 in 8.1 s`,
`summary ok=35 fail=0 unsupported=0`, `peak RSS 6.25 GiB`.

Device line:
`PlayStation 5 GPU (RADV NAVI21) (radv) | uma: 1 | fp16: 1 | bf16: 0 | fp4: 0 | warp size: 32 | shared memory: 65536 | int dot: 0 | matrix cores: none`.

So the coopmat question **does not apply on this driver**: `matrix cores: none`,
and the bench says so itself — `coopmat on/off: the driver exposes no
cooperative matrix, so both settings are the same path; single pass`. There is no
side-by-side block and no second pass (35, not 70, as `spike-a.md` §7 explains).
`stem parity coopmat=off: F32 im2col nmse 6.82e-08, ggml_conv_2d nmse 1.65e-05
(242.2x worse) -> use F32 im2col` repeats the desktop conclusion on RADV.
