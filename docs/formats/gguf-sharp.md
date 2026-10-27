# SHARP checkpoint → GGUF, converter version 1

C1 converts Apple's SHARP checkpoint into one GGUF file that the C3 ggml backend
loads. The converted file is a **model derivative** under Apple's
`LICENSE_MODEL`: dtype and layout change, tensors are dropped, and the file is
labelled as converted with the changes listed inside it (spec rules 1 and 4).
It is never committed or shipped; `splat_convert` writes it on the user's
machine after the licence gate and the hash check.

Everything below was measured with this repository's converter on the real
checkpoint.

## Source checkpoint

| | |
| --- | --- |
| URL | `https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt` |
| Size | 2,809,738,232 bytes |
| SHA-256 | `94211a75198c47f61fca7d739ba08a215418d8d398d48fddf023baccc24f073d` |
| Container | zip (torch.save v1 zipfile), **zip64** end-of-central-directory record |
| Tensors | 1,038, all `torch.float32`, one storage per tensor, contiguous, offset 0 |
| Tensor bytes | 2,809,220,676 |

The hash and the byte count are constants in `core/model/checkpoint.h`
(`kSharpCheckpointSha256`, `kSharpCheckpointBytes`). `splat_convert` refuses to
convert anything else, which is spec rule 2. `.pt` is a zip: the pickle is
`<prefix>/data.pkl` and each tensor blob is `<prefix>/data/<storage key>`.

torch.save writes the zip64 end record even though every entry is below 4 GiB,
so the checkpoint is read through miniz's zip64 path. Entry data is streamed in
miniz's 64 KiB read buffer; the 2.6 GB file is never mapped or buffered.

## Converter

```
splat_convert <in.pt> <out.gguf>
```

Exit status 0 on success, 1 on a conversion error (hash mismatch, unsupported
pickle, corrupt zip), 2 on a usage error. It prints the verified hash, the zip
flavour, tensor counts, file size, seconds and peak RSS.

Measured on this host (`/usr/bin/time -l`, Release build):

| | |
| --- | --- |
| Output | 1,312,705,920 bytes (1.22 GiB) |
| Tensors written | 878 of 1,038 |
| Wall time | 12.6 s (about half of it the 2.6 GB SHA-256 pass) |
| Peak RSS | 17.0 MiB |

The real-checkpoint test converts into
`weights/sharp_2572gikvuh-f16-v1.gguf` and reuses that file on later runs, so a
rebuild does not pay for the 1.3 GB write again; delete it to force a fresh
conversion.

The streaming design keeps memory flat: the header is written with
`gguf_write_to_file_ptr(ctx, file, only_meta = true)`, then each payload is read
from the zip and appended with bounded chunks (`GgufWriter::begin_tensor` /
`append_tensor_bytes` / `end_tensor`). Only the current chunk and a small
carry buffer are alive, so peak cost is the 16 MiB ggml metadata arena plus
buffers — far below "largest tensor + 256 MiB".

## File layout

A plain GGUF v3 file as written by ggml (`third_party/ggml/src/gguf.cpp`):

```
offset 0                 magic "GGUF", version 3 (u32), tensor count (u64), kv count (u64)
                        key-value block (15 entries)
                        tensor info block: name, rank, ne[], type, offset
                        zero padding to the 32-byte alignment
data section             878 tensor payloads, each padded to 32 bytes
```

Measured for the real checkpoint: the tensor info block ends at byte 157,137, the
data section starts at byte 157,152 (aligned to 32), and the data section is
1,312,548,768 bytes. `gguf_get_tensor_offset()` returns offsets relative to the
start of the data section; `gguf_get_data_offset()` returns 157,152.

GGUF does not compress tensor data. fp16 halves the model and is all this format
offers per tensor.

## Metadata

| Key | Type | Value |
| --- | --- | --- |
| `general.architecture` | string | `sharp` |
| `general.name` | string | `SHARP (Apple ml-sharp 2572gikvuh)` |
| `sharp.backend` | string | backend name for `BackendInfo::name`, `sharp` |
| `sharp.converter` | string | `splat_convert` |
| `sharp.converter_version` | u32 | `1` |
| `sharp.source_sha256` | string | SHA-256 of the `.pt` that was converted |
| `sharp.source_filename` | string | base name of the input |
| `sharp.source_bytes` | u64 | input size in bytes |
| `sharp.source_tensor_count` | u32 | `1038` |
| `sharp.tensor_count` | u32 | `878`, must equal the GGUF tensor count |
| `sharp.dropped_tensor_count` | u32 | `160` |
| `sharp.dropped_tensors` | string array | torch names of the dropped tensors |
| `sharp.source_tensor_names` | string array | full torch name of every GGUF tensor, in tensor order |
| `sharp.tensor_name_rule` | string | explains the name shortening below |
| `sharp.changes` | string array | the derivative label: dtype, layout and drop list |

`sharp.changes` currently holds four lines:

1. `dtype: tensors with 2 or more dimensions converted from float32 to float16`
2. `dtype: 1-D tensors (norm weights, norm biases, conv biases) kept as float32`
3. `layout: torch shape (d0..dn) stored as ggml ne (dn..d0); bytes unchanged, row-major element order preserved, no transpose applied`
4. `dropped: 2 ViT classifier heads and the depth_alignment.* U-Net, which the reference never runs at inference`

## Tensor names

ggml caps tensor names at 63 bytes (`GGML_MAX_NAME` is 64 including the NUL) and
the torch names reach 84 characters, so the constant module prefix is stripped:

```
monodepth_model.monodepth_predictor.<rest>  ->  <rest>
everything else                             ->  unchanged
```

Longest resulting name is 57 bytes. Names never collide because only a constant
prefix is removed. The full torch name is still available:
`sharp.source_tensor_names[i]` belongs to GGUF tensor `i`. Layers that C3 looks
up by hand:

| Torch name | GGUF name |
| --- | --- |
| `monodepth_model.monodepth_predictor.encoder.patch_encoder.blocks.23.attn.proj.weight` | `encoder.patch_encoder.blocks.23.attn.proj.weight` |
| `monodepth_model.monodepth_predictor.decoder.convs.1.weight` | `decoder.convs.1.weight` |
| `feature_model.decoder.convs.0.weight` | `feature_model.decoder.convs.0.weight` |
| `prediction_head.bias` | `prediction_head.bias` |

## Dtypes and shapes

* **2+ dimensions → fp16** (`GGML_TYPE_F16`): 302 tensors, 654,757,312 values,
  1,309,514,624 bytes.
* **1 dimension → kept fp32** (`GGML_TYPE_F32`): 576 tensors, 758,526 values,
  3,034,104 bytes. These are LayerNorm weights/biases and conv biases; rounding
  them costs accuracy for no size win.
* Sum: 1,312,548,728 bytes of payload, 1,312,548,768 after 32-byte padding.

Torch stores shapes outermost-first; GGUF/ggml store `ne[0]` as the
fastest-moving dimension, so the shape is reversed and the rank is preserved:

```
torch (128, 256, 1, 1)  ->  ne [1, 1, 256, 128], rank 4
torch (4096, 1024)      ->  ne [1024, 4096],      rank 2
torch (1024,)           ->  ne [1024],            rank 1
```

The payload bytes are **not** touched: both layouts are row-major over the same
linear element order, so this is a metadata reinterpretation, not a transpose.
A linear index `i` (torch C order) is byte offset `i * itemsize` in the GGUF
tensor, which is what the tests use for spot checks.

Note for C3: `nn.Linear` weights therefore still need the ggml-side transpose
(`ggml_mul_mat` wants `ne = [in, out]`; the reference transposes per call, see
`docs/research/sharp-reference.md` §11.5). Conversion does not pre-transpose
anything, so the file stays a faithful image of the checkpoint.

fp32 → fp16 is round-to-nearest-even from `core/common/half.h`, identical on
every platform. Relative error is bounded by 2^-11 (about 5e-4); the tests
accept 2e-3.

## Dropped tensors

160 tensors, 46,789,331 parameters (187 MB in fp32), all dead at
inference per `docs/research/sharp-reference.md` §2 and §3 Q1:

| Group | Tensors | Params | Why |
| --- | --- | --- | --- |
| `depth_alignment.*` | 156 | 2,015,281 | the U-Net never runs: `DepthAlignment.forward` returns a ones map at inference |
| `...encoder.patch_encoder.head.{weight,bias}` | 2 | 22,387,025 | timm ImageNet classifier head, `TimmViT.forward` stops before it |
| `...encoder.image_encoder.head.{weight,bias}` | 2 | 22,387,025 | same |

Their names are listed in `sharp.dropped_tensors` so a reader can tell exactly
what was removed.

## Reading the file

```cpp
gguf_init_params params{};
params.no_alloc = true;   // tensor table only, no 1.3 GB allocation
params.ctx = &ggml;       // ggml_init'ed context for the tensor structs
gguf_context* gguf = gguf_init_from_file(path, params);
// gguf_find_tensor(gguf, "encoder.patch_encoder.blocks.0.attn.proj.weight")
// gguf_get_tensor_ne / _type / _offset / _size
```

Validation a reader may rely on:

1. `magic == "GGUF"`, `version == 3`;
2. `sharp.tensor_count` equals `gguf_get_n_tensors()`;
3. every `sharp.source_tensor_names[i]` exists once, and
   `gguf_get_tensor_offset(i)` is monotonic and 32-byte aligned;
4. every tensor is F16 with rank ≥ 2 or F32 with rank 1.

A mismatch in (2) or (3) means the file was truncated or edited and should be
rejected; `splat_convert` output always satisfies all four.

## Companion state: `licenses.json`

The licence gate (spec rule 3) records acceptance in
`<state dir>/licenses.json`; `core/model/license.h` reads and writes it:

```json
{
  "format_version": 1,
  "accepted": [
    {
      "backend": "sharp",
      "license_sha256": "9c1f…",
      "accepted_at": "2026-10-25T18:03:11Z"
    }
  ]
}
```

`license_sha256` is the SHA-256 of the exact `LICENSE_MODEL` text that was shown,
so a changed licence invalidates the old acceptance. One entry per backend;
recording again replaces it. A missing file means nothing has been accepted yet
(not an error); a malformed file is an error. Writes go through
`licenses.json.tmp` and a rename.
