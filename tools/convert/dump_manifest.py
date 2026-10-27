#!/usr/bin/env python3
"""Dumps the tensor manifest the real-checkpoint test compares against.

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}   # the repo parent by default
    "$DATA"/ref/venv-sharp/bin/python tools/convert/dump_manifest.py \
        --checkpoint "$DATA"/weights/sharp_2572gikvuh.pt \
        --out "$DATA"/runs/sharp-manifest.json

The manifest is written outside the repo on purpose: it describes a 2.6 GB
model derivative and the repository never carries weights (spec rule 1). The
JSON schema is the same as tests/fixtures/model/synthetic.json, so
tests/model_convert_test.cpp reads both with one code path.

For every tensor kept by the converter: the torch state-dict name, the GGUF
tensor name (the "monodepth_model.monodepth_predictor." prefix is stripped to
fit ggml's 63-byte limit), the shape, the expected GGUF dtype, and a handful of
spot values at fixed linear indices. Linear order is the same in both files, so
a spot value is one seek into the GGUF data section.
"""
import argparse
import hashlib
import json
import os
import sys

import torch

TORCH_PREFIX = "monodepth_model.monodepth_predictor."

# AGENTS.md: large material lives outside the repo, under $SPLAT_DATA_DIR
# (default: this repo's parent directory).
DATA_DIR = os.environ.get("SPLAT_DATA_DIR") or os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
DEFAULT_CHECKPOINT = os.path.join(DATA_DIR, "weights", "sharp_2572gikvuh.pt")
DEFAULT_OUT = os.path.join(DATA_DIR, "runs", "sharp-manifest.json")
PINNED_SHA256 = "94211a75198c47f61fca7d739ba08a215418d8d398d48fddf023baccc24f073d"


def gguf_name(name):
    return name[len(TORCH_PREFIX):] if name.startswith(TORCH_PREFIX) else name


def is_dead(name):
    if name.startswith("depth_alignment."):
        return True
    return any(name.endswith(suffix) for suffix in (
        "encoder.patch_encoder.head.weight", "encoder.patch_encoder.head.bias",
        "encoder.image_encoder.head.weight", "encoder.image_encoder.head.bias"))


def spot_indices(count):
    wanted = {0, 1, 2, count // 2, count - 2, count - 1}
    return [i for i in sorted(wanted) if 0 <= i < count]


def file_sha256(path, chunk=1 << 20):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(chunk)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", default=DEFAULT_CHECKPOINT)
    parser.add_argument("--out", default=DEFAULT_OUT)
    args = parser.parse_args()

    digest = file_sha256(args.checkpoint)
    size = os.path.getsize(args.checkpoint)
    print(f"{args.checkpoint}: {size} bytes, sha256 {digest}")
    if digest != PINNED_SHA256:
        print("warning: hash does not match the pinned checkpoint", file=sys.stderr)

    state = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    dropped = [name for name in state if is_dead(name)]

    tensors = []
    for name, tensor in state.items():
        if is_dead(name):
            continue
        flat = tensor.reshape(-1)
        indices = spot_indices(flat.numel())
        tensors.append({
            "name": name,
            "gguf_name": gguf_name(name),
            "shape": list(tensor.shape),
            "dtype": "float16" if tensor.dim() >= 2 else "float32",
            "spot_indices": indices,
            "spot_values": [float(flat[i]) for i in indices],
        })

    document = {
        "source": os.path.basename(args.checkpoint),
        "checkpoint_sha256": digest,
        "checkpoint_bytes": size,
        "tensor_count": len(state),
        "converted_tensor_count": len(tensors),
        "dropped_count": len(dropped),
        "dropped": dropped,
        "tensor_order": list(state.keys()),
        "tensors": tensors,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as handle:
        json.dump(document, handle, indent=1)
        handle.write("\n")
    print(f"wrote {args.out}: {len(tensors)} tensors kept, {len(dropped)} dropped")


if __name__ == "__main__":
    main()
