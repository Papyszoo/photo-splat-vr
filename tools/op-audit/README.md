# tools/op-audit — SHARP-shaped ggml op audit

The harness that produced `docs/research/op-audit.md`: it builds each ggml op at
the shapes SHARP uses at 1536×1536, checks `ggml_backend_supports_op` on the
Vulkan device, runs it there, and compares against a CPU (or fp64 host)
reference with max-abs, NMSE and cosine.

It is a **manual** tool, not part of the default build, and it needs the same
checkout of `third_party/ggml` the app uses.

| File | Question it answers |
| --- | --- |
| `sharp_ops.cpp` | every SHARP op, at the real tensor shapes, on Vulkan vs CPU |
| `conv_probe.cpp` | is `ggml_conv_2d` accurate? (the F16 im2col trap) |
| `conv_probe_direct.cpp` | the same conv as an explicit im2col + matmul |
| `conv_probe_st.cpp` | staged/im2col variant of the probe |
| `mm_probe.cpp` | matmul shape sweep for the ViT projections |

## Build

```bash
cmake -B build-audit -DSPLAT_BUILD_OP_AUDIT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-audit --target sharp_ops
```

The binaries land in `build-audit/tools/op-audit/`. They link `splat_core`, so
they see the same ggml and the same Vulkan backend the renderer uses.

## Run

```bash
# every case, Vulkan device 0 vs CPU
build-audit/tools/op-audit/sharp_ops

# one case (substring match), or list the case names
build-audit/tools/op-audit/sharp_ops conv
build-audit/tools/op-audit/sharp_ops --list
```

Output is one line per case: `case`, `vk` (OK/UNSUP/FAIL), node count, Vulkan
ms, `max_abs`, `NMSE`, `cos`, and a note. The exit code is non-zero when any
case fails.

## Results so far (MoltenVK, Apple M4)

31/31 exact-shape cases are supported and correct; 735/981/258/440 stock
`test-backend-ops` cases pass. Two findings moved into the port:

- `ggml_conv_2d` stages patches in F16 (NMSE 1.7e-5, max abs 0.127 at the ViT
  stem). Use F32 im2col (27 ms) or direct conv (93 ms) for parity.
- RADV's coopmat conv may accumulate in F16.

## Must be re-run on RADV / PS5

The MoltenVK numbers are a convenience baseline, not a parity target. Before
the Spike A gate, re-run `sharp_ops` and `conv_probe*` on the desktop RADV build
and then on the console, and compare the notes column: a case that is "UNSUP"
or drifts above 1e-3 max relative error on RADV is a port decision, not a
driver detail.

Build on Linux/AMD as above (the Docker PS5 toolchain also works); on the
console the harness runs as a payload with the Vulkan backend already
initialised, so `ggml_backend_load_all()` is a no-op there.
