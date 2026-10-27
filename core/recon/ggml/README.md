# core/recon/ggml — shared ggml blocks (C3/C4)

The pieces both reconstruction backends share: backend selection, GGUF weights,
the graph-splitting helper, the DINOv2 ViT encoder, the DPT neck/head, and the
Depth Anything V2 driver. Nothing in here is DA2-specific beyond the config
values that `DepthAnythingV2::initialize()` fills in, so the SHARP port reuses
the same blocks with `ViTConfig{dim 1024, 16 heads, 24 blocks, patch 16}` and
`DptConfig{4 levels, 256 features}` plus its own `ViTNames`/`DptNames`.

| File | Contents |
| --- | --- |
| `backend.h/.cpp` | `GgmlBackend`: Vulkan when a device initializes, CPU otherwise. `create(prefer_vulkan, &note)`. Console always Vulkan; this Mac uses MoltenVK; CI uses CPU. |
| `weights.h/.cpp` | `GgmlWeights`: reads a GGUF, recreates its tensors in one backend buffer, copies every tensor, keeps the string metadata. `find`/`need` by GGUF name, `download` (f32/f16 → host f32). |
| `graph.h/.cpp` | `GraphSession`: builds the graph in segments and submits each segment with one `ggml_backend_graph_compute`. Tensors crossing a boundary are copied into a persistent arena (`ggml_tallocr`) that outlives the per-submit allocation. |
| `check.h` | `require(condition, message)`: the validation both backends' graph builders throw with. `run()` turns it into a `Status`. |
| `stage.h` | `StageDump`/`StageCallback`: one tensor read back from a stage for a parity test. The DA2 driver (`DepthAnythingStage`) and the SHARP stages (`SharpStage`) are aliases of it. |
| `nn.h/.cpp` | Conv2d / ConvTranspose2d / Linear / LayerNorm / bias-add over channels-last maps, plus token ↔ map conversion. |
| `pos_embed.h/.cpp` | Bicubic position-embedding resample on the host, matching `F.interpolate(..., mode="bicubic", align_corners=False)` with the DINOv2 `interpolate_offset`. |
| `vit.h/.cpp` | Configurable DINOv2 ViT: patch embed (native `CONV_2D`), pos-embed add, block (LayerNorm → q/k/v → `FLASH_ATTN_EXT` → proj → LayerScale → MLP GELU-erf → LayerScale), final norm. Fused-qkv (timm) and separate q/k/v (HF) checkpoints both supported, single image or a batch of images. |
| `dpt.h/.cpp` | DPT reassemble (1x1 + TConv/identity/3x3-s2), 4 `FeatureFusionBlock`s (residual 3x3 units, bilinear align_corners upsample, 1x1) and the depth head. `resolve_conv` (weight required, bias optional) and `require` are shared with the SHARP decoders; `fusion_block`/`resolve_fusion_block` are the SHARP variant, which upsamples with a 2x2/2 TConv inside the block. |
| `depth_anything.h/.cpp` | C4 driver: config check against GGUF metadata, taps at blocks 2/5/8/11, stage dumping for parity, cv2-compatible preprocessing (`da2_preprocess`). |

The SHARP port (`core/recon/sharp`) is the second user of these blocks and adds
only what ViT-L/16 needs: `ViTConfig{1024, 16 heads, 24 blocks, patch 16}` with
`layer_scale`/`combined_qkv` on, `ViTNames::prefix` (SHARP stores the two ViTs
under `encoder.patch_encoder.` and `encoder.image_encoder.`), the timm LayerScale
spelling `ls1.gamma`/`ls2.gamma`, and **batched** token tensors
`(dim, tokens, batch)` — `vit_block` takes a batch in `ne2` and
`vit_patch_embed`/`map_to_tokens` pass a batch through the patch-embed conv, so
35 patches encode in sub-batches of 4. With the checkpoint's own position grid
(`out_w/out_h == grid_w/grid_h`) `vit_add_position` adds the learned embedding
directly, as timm does, instead of resampling it.

`nn::linear` sets `GGML_PREC_F32` on both the accumulator and src1. That is what
keeps an fp16-weight GGUF inside the spec's cosine gate on ggml-vulkan: the
backend's default for non-F32 weights is an fp16 accumulator *and* fp16
activations (`ggml_vk_get_mul_mat_mat_f16acc`, `prefer_f16_b`), which over 24 ViT
blocks costs 5-30x more error (0.9986 instead of 0.9999991 on SHARP's level 2)
and takes the patch encoder from 14.9 s to 9.6 s when left alone on MoltenVK.
Revisit on RADV, where cooperative matrices may make the fp32 path relatively
more expensive.


## Why the graph is split

Spec risk: "Long GPU submissions hit a timeout (device lost)" — RADV loses the
device on long compute jobs (llama.cpp #21724). ggml-vulkan also splits submits
internally (`GGML_VK_MAX_NODES_PER_SUBMIT`), but that is backend-internal and
does not bound the *graph* a caller builds.

`GraphSession` bounds it from above:

```cpp
GraphSession session(backend, weights, /*nodes_per_submit=*/64, /*arena_bytes=*/384<<20);
ggml_tensor* x = ...;                  // ops in session.ctx()
x = session.checkpoint(x);             // submit only if the segment is over budget
x = session.carry(x);                  // force a boundary (stage output)
x = session.finish(x);                 // last submit, returns the carried result
```

`checkpoint()` is called at the end of every ViT block and DPT stage; `carry()`
is used where the caller needs a stable tensor (parity dumps, tap maps). A DA2
run at 924x518 does 28 submits, so no single submission is long.

## Depth Anything V2 stage names

`DepthAnythingV2::run(input_chw, w, h, stage_cb)` reports these tensors (ggml
`ne` order, i.e. the transpose of the torch names below); the parity test maps
them 1:1 onto `golden/da2/<photo>/`:

| Stage | Shape (ggml) | Golden file (torch) |
| --- | --- | --- |
| `patch_embed` | (384, npatch) | `020_patch_embed.npy` (1, npatch, 384) |
| `block_02` / `05` / `08` / `11` | (384, 1+npatch) | `030_block_XX.npy` |
| `tap_patch_00..03` | (384, npatch) | `040_tap_patch_XX.npy` |
| `layer_00..03` | (W, H, C) | `050_layer_XX.npy` (1, C, H, W) |
| `path_4..1` | (W, H, 64) | `060_path_N.npy` |
| `conv1` | (W, H, 32) | `061_conv1.npy` |
| `depth` | (W, H, 1, 1) | `070_depth_net.npy` (H, W) |

`pos_embed` is the one stage computed on the host (`resample_pos_embed`); the
parity test compares it against `021_pos_embed.npy` directly.

## Running the tests with data

The data lives outside the repo (AGENTS.md), so these cases skip when it is
absent:

```bash
export SPLAT_DATA_DIR=/path/to/data          # golden/, weights/, runs/ (repo parent by default)
export SPLAT_DA2_GOLDEN="$SPLAT_DATA_DIR/golden/da2"
export SPLAT_DA2_GGUF="$SPLAT_DATA_DIR/weights/dav2s-f32.gguf"        # fp32 parity
export SPLAT_DA2_GGUF_F16="$SPLAT_DATA_DIR/weights/dav2s-f16.gguf"    # shipped runtime file
export SPLAT_FALLBACK_PHOTO="$SPLAT_DATA_DIR/golden/immichvr/4.jpg"
build-W3-fallback/tests/splat_tests -tc="da2 parity*,fallback*,ggml*"
```

Gates: fp32 CPU p99 relative error ≤ 1e-3 and p99.9 ≤ 2e-3 with max abs ≤ 1e-3
per stage; fp16 (spec gate) cosine ≥ 0.999 per stage; Vulkan is reported but not
gated, since the spec excludes MoltenVK from parity.

## Converting the weights

```bash
build-W3-fallback/tools/convert/splat_convert --backend da2 \
    "$SPLAT_DATA_DIR"/weights/dav2s.safetensors \
    "$SPLAT_DATA_DIR"/weights/dav2s-f16.gguf
# --fp32 keeps every tensor float32; that file is the parity target.
```

Pinned identity (verified before conversion, spec rule 2 / C4):

```
https://huggingface.co/depth-anything/Depth-Anything-V2-Small-hf/resolve/main/model.safetensors
sha256 3152477ce0d8d6978d76b995120de97cb5b928701fd0f817769f59e249a16b70
99,173,660 bytes, Apache-2.0, 287 tensors, 24.79 M parameters
```

## Measured parity and cost on this Mac

Apple M4 (10-core, 16 GiB), macOS 26.5, MoltenVK 1.4.363, ggml 0.27.0. Worst
case over the five golden photos (`photo01..03` + `immichvr/4,5`), relative
error taken over `|ref| > 1e-2`.

fp32 GGUF, CPU backend (the gate):

| Stage | max abs | max rel | p99.9 rel | cos |
| --- | --- | --- | --- | --- |
| patch_embed | 1.9e-06 | 4.2e-05 | 1.2e-05 | 1.0000000 |
| block_02 | 2.8e-06 | 1.5e-04 | 4.7e-05 | 1.0000000 |
| block_05 | 5.0e-06 | 3.7e-04 | 1.0e-04 | 1.0000000 |
| block_08 | 2.7e-05 | 8.5e-04 | 2.1e-04 | 1.0000000 |
| block_11 | 1.1e-04 | 3.9e-03 | 4.6e-04 | 1.0000000 |
| tap_patch_00..03 | 5.6e-04 | 2.0e-02 | 1.2e-03 | 1.0000000 |
| layer_00..03 | 2.9e-04 | 8.0e-03 | 9.2e-04 | 1.0000000 |
| path_4..1 | 2.8e-04 | 1.0e-02 | 6.1e-04 | 1.0000000 |
| conv1 | 4.4e-04 | 1.5e-02 | 5.8e-04 | 1.0000000 |
| depth | 5.1e-04 | 7.4e-03 | 1.2e-04 | 1.0000000 |

The `max rel` column is dominated by isolated near-zero elements (the reference
itself shows 1.8e-3 between two PyTorch backends, see
`docs/research/sharp-reference.md` §10), which is why the gate is on p99/p99.9
plus max abs.

fp16 GGUF (the shipped runtime file), CPU: cosine 0.99998-0.99999 on every
stage; `depth` p99 rel 7.4e-03, max abs 9.4e-02 on values up to ~8, i.e. ~1% of
the relative-inverse-depth scale, which the assumed-depth mapping absorbs.

Vulkan (MoltenVK, fp32): cosine 0.99999 on every stage, depth max abs 2.1e-02,
but per-element relative error up to ~0.1 in the ViT taps from accumulation
order. Reported, not gated: the spec excludes MoltenVK from parity, repeat this
on RADV.

End to end through `splat_reconstruct` (job time includes JPEG decode, EXIF
focal, inference, splat assembly and the scene-folder write):

| Photo | Size | Splats | Vulkan job | CPU job | Peak RSS |
| --- | --- | ---: | ---: | ---: | ---: |
| photo01_living_room | 1280x873 | 499,897 | 0.62 s | 1.10 s | 198 / 268 MiB |
| photo02_street | 1280x720 | 497,103 | 0.40 s | 1.32 s | 197 / 307 MiB |
| immichvr/4 | 1440x1920 | 499,215 | 0.29 s | 0.93 s | 201 / 266 MiB |

The DA2 graph alone is 0.32 s on Vulkan and 0.7-1.1 s on CPU per photo, in 28
submits, with a 117-158 MiB cross-submit arena at 924x518 (the 384 MB arena is
reserved, only the touched pages are resident).
