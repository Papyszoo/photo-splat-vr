#!/usr/bin/env python3
"""Dump Depth Anything V2 Small reference stage outputs ("golden") for the C4 port.

Runs the official PyTorch reference (github.com/DepthAnything/Depth-Anything-V2,
cloned into ``ref/Depth-Anything-V2`` outside this repo) with the official
Apache-2.0 ``Depth-Anything-V2-Small-hf`` safetensors checkpoint and writes the
stage inputs/outputs the ggml port must reproduce as ``.npy``.

The checkpoint is the Hugging Face ``transformers`` conversion, so its tensor
names differ from the reference repo's state dict (``backbone.*`` / ``neck.*`` /
``head.*`` versus ``pretrained.*`` / ``depth_head.*``). ``hf_state_dict`` below
performs the mapping; the four separate HF q/k/v projections are concatenated
into the repo's fused ``qkv`` weight so the loaded model is the reference graph.

Stages (C4 numbering; ``docs/research/op-audit.md`` §3.5):

    010_input          normalised network input, 1x3xHxW (Resize 518 short side
                       + ImageNet normalise; cv2 INTER_CUBIC, exactly as
                       DepthAnythingV2.image2tensor)
    020_patch_embed    patch_embed conv output, 1 x (ph*pw) x 384, before cls/pos
    021_pos_embed      bicubic-interpolated position embedding, 1 x (1+ph*pw) x 384
    030_block_XX       full token tensor after encoder blocks 2, 5, 8 and 11
    040_tap_patch_XX   post-norm patch tokens handed to the DPT head, 1 x ph*pw x 384
    040_tap_cls_XX     post-norm cls token, 1 x 384
    050_layer_XX       DPT reassemble stage output (after 1x1 project + resize),
                       1 x C x h x w for the 1/4, 1/8, 1/16 and 1/32 levels
    060_path_X         FeatureFusionBlock outputs (path_4 .. path_1)
    061_conv1          output_conv1 (64 -> 32) at 1/3.5 input resolution
    070_depth_net      final relative inverse depth at network resolution, HxW
    071_depth_full     the same, bilinearly resized to the photo, hxw
                       (align_corners=True, as ``infer_image`` does)

Example:

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}   # the repo parent by default
    V="$DATA"/ref/venv-sharp/bin
    $V/python tools/golden/dump_da2_golden.py \
        --weights "$DATA"/weights/dav2s.safetensors \
        --repo "$DATA"/ref/Depth-Anything-V2 \
        --out "$DATA"/golden/da2 \
        --photos "$DATA"/golden/inputs/*.jpg \
                 "$DATA"/golden/immichvr/4.jpg \
                 "$DATA"/golden/immichvr/5.jpg

Everything this writes stays outside the repo (``$SPLAT_DATA_DIR/golden/``);
``*.npy`` is gitignored.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import resource
import struct
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image, ImageOps

# --------------------------------------------------------------------------- #
# checkpoint


def read_safetensors(path: Path) -> dict[str, np.ndarray]:
    """Minimal safetensors reader: header JSON + raw little-endian blobs."""
    with path.open("rb") as handle:
        header_len = struct.unpack("<Q", handle.read(8))[0]
        header = json.loads(handle.read(header_len))
        base = 8 + header_len
        tensors: dict[str, np.ndarray] = {}
        for name, info in header.items():
            if name == "__metadata__":
                continue
            if info["dtype"] != "F32":
                raise SystemExit(f"{path}: {name} is {info['dtype']}, only F32 is supported")
            begin, end = info["data_offsets"]
            handle.seek(base + begin)
            raw = handle.read(end - begin)
            shape = tuple(info["shape"])
            tensors[name] = np.frombuffer(raw, dtype="<f4").reshape(shape).copy()
    return tensors


def hf_state_dict(hf: dict[str, np.ndarray]) -> dict[str, torch.Tensor]:
    """Map the HF DepthAnythingForDepthEstimation tensors to the reference keys."""
    out: dict[str, torch.Tensor] = {}

    def put(key: str, value) -> None:
        out[key] = torch.from_numpy(np.ascontiguousarray(value, dtype=np.float32))

    for name, value in hf.items():
        if name == "backbone.embeddings.cls_token":
            put("pretrained.cls_token", value)
        elif name == "backbone.embeddings.mask_token":
            put("pretrained.mask_token", value)  # unused (masks are None) but part of the module
        elif name == "backbone.embeddings.patch_embeddings.projection.weight":
            put("pretrained.patch_embed.proj.weight", value)
        elif name == "backbone.embeddings.patch_embeddings.projection.bias":
            put("pretrained.patch_embed.proj.bias", value)
        elif name == "backbone.embeddings.position_embeddings":
            put("pretrained.pos_embed", value)
        elif name == "backbone.layernorm.weight":
            put("pretrained.norm.weight", value)
        elif name == "backbone.layernorm.bias":
            put("pretrained.norm.bias", value)
        elif name.startswith("backbone.encoder.layer."):
            rest = name[len("backbone.encoder.layer."):]
            index, _, tail = rest.partition(".")
            prefix = f"pretrained.blocks.{index}."
            simple = {
                "norm1.weight": "norm1.weight",
                "norm1.bias": "norm1.bias",
                "norm2.weight": "norm2.weight",
                "norm2.bias": "norm2.bias",
                "attention.output.dense.weight": "attn.proj.weight",
                "attention.output.dense.bias": "attn.proj.bias",
                "layer_scale1.lambda1": "ls1.gamma",
                "layer_scale2.lambda1": "ls2.gamma",
                "mlp.fc1.weight": "mlp.fc1.weight",
                "mlp.fc1.bias": "mlp.fc1.bias",
                "mlp.fc2.weight": "mlp.fc2.weight",
                "mlp.fc2.bias": "mlp.fc2.bias",
            }
            if tail in simple:
                put(prefix + simple[tail], value)
            elif tail.startswith("attention.attention."):
                continue  # the four HF q/k/v projections are fused into qkv below
            else:
                raise SystemExit(f"unmapped checkpoint tensor {name}")
        elif name == "head.conv1.weight" or name == "head.conv1.bias":
            put("depth_head.scratch.output_conv1." + name.split(".")[-1], value)
        elif name in ("head.conv2.weight", "head.conv2.bias", "head.conv3.weight", "head.conv3.bias"):
            # output_conv2 is Sequential(conv3x3, relu, conv1x1, relu, identity)
            index = 0 if name.startswith("head.conv2") else 2
            put(f"depth_head.scratch.output_conv2.{index}." + name.split(".")[-1], value)
        elif name.startswith("neck.convs."):
            # neck.convs.i -> depth_head.scratch.layer{i+1}_rn
            index = name.split(".")[2]
            put(f"depth_head.scratch.layer{int(index) + 1}_rn." + ".".join(name.split(".")[3:]), value)
        elif name.startswith("neck.reassemble_stage.layers."):
            parts = name.split(".")
            index = parts[3]
            if parts[4] == "projection":
                put(f"depth_head.projects.{index}." + ".".join(parts[5:]), value)
            elif parts[4] == "resize":
                put(f"depth_head.resize_layers.{index}." + ".".join(parts[5:]), value)
            else:
                raise SystemExit(f"unmapped checkpoint tensor {name}")
        elif name.startswith("neck.fusion_stage.layers."):
            parts = name.split(".")
            index = parts[3]  # HF layers are numbered deep -> shallow
            tail = ".".join(parts[4:])
            # HF fusion_stage.layers.0 handles the deepest level, which is the
            # reference's refinenet4 (DepthAnythingFeatureFusionStage reverses
            # the feature list before zipping with its layers).
            refinenet = f"depth_head.scratch.refinenet{4 - int(index)}"
            if tail.startswith("projection."):
                put(f"{refinenet}.out_conv." + ".".join(parts[5:]), value)
            elif parts[4].startswith("residual_layer"):
                unit = parts[4][-1]  # residual_layer1 -> ResUnit1
                conv = parts[5][-1]  # convolution1 -> conv1
                put(f"{refinenet}.resConfUnit{unit}.conv{conv}." + parts[6], value)
            else:
                raise SystemExit(f"unmapped checkpoint tensor {name}")
        else:
            raise SystemExit(f"unmapped checkpoint tensor {name}")

    # Fuse the four separate HF block projections into the reference qkv weight.
    for index in range(12):
        prefix = f"pretrained.blocks.{index}."
        for suffix in ("weight", "bias"):
            parts = [hf[f"backbone.encoder.layer.{index}.attention.attention.{kind}.{suffix}"]
                     for kind in ("query", "key", "value")]
            out[prefix + f"attn.qkv.{suffix}"] = torch.from_numpy(
                np.ascontiguousarray(np.concatenate(parts, axis=0), dtype=np.float32))
    return out


# --------------------------------------------------------------------------- #
# dumps


def as_numpy(tensor: torch.Tensor) -> np.ndarray:
    return tensor.detach().to("cpu", dtype=torch.float32).numpy()


def peak_rss_gib() -> float:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2**30


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_photo(path: Path) -> np.ndarray:
    """RGB uint8, EXIF orientation applied, alpha dropped (the C2 Image contract)."""
    with Image.open(path) as image:
        image = ImageOps.exif_transpose(image)
        if image.mode != "RGB":
            image = image.convert("RGB")
        return np.asarray(image, dtype=np.uint8)


def dump_photo(model, image_path: Path, out_root: Path, input_size: int) -> dict:
    from depth_anything_v2.util.transform import NormalizeImage, PrepareForNet, Resize
    from torchvision.transforms import Compose

    photo_id = image_path.stem
    folder = out_root / photo_id
    folder.mkdir(parents=True, exist_ok=True)

    rgb = load_photo(image_path)
    original_h, original_w = rgb.shape[:2]

    transform = Compose([
        Resize(width=input_size, height=input_size, resize_target=False, keep_aspect_ratio=True,
               ensure_multiple_of=14, resize_method="lower_bound",
               image_interpolation_method=3),  # cv2.INTER_CUBIC
        NormalizeImage(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
        PrepareForNet(),
    ])
    image = transform({"image": rgb.astype(np.float32) / 255.0})["image"]
    net_input = torch.from_numpy(image).unsqueeze(0)
    net_h, net_w = net_input.shape[-2:]
    patch_h, patch_w = net_h // 14, net_w // 14

    records: dict[str, torch.Tensor] = {}
    handles = []

    def hook(name):
        def record(_module, _args, output):
            records[name] = output
        return record

    handles.append(model.pretrained.patch_embed.proj.register_forward_hook(hook("patch_embed")))
    for index in (2, 5, 8, 11):
        handles.append(model.pretrained.blocks[index].register_forward_hook(hook(f"block_{index}")))
    for index in range(4):
        handles.append(model.depth_head.projects[index].register_forward_hook(hook(f"project_{index}")))
        handles.append(model.depth_head.resize_layers[index].register_forward_hook(hook(f"resize_{index}")))
    for index in range(4):
        refinenet = getattr(model.depth_head.scratch, f"refinenet{4 - index}")
        handles.append(refinenet.register_forward_hook(hook(f"path_{4 - index}")))
    handles.append(model.depth_head.scratch.output_conv1.register_forward_hook(hook("conv1")))

    started = time.time()
    with torch.no_grad():
        depth = model.forward(net_input)  # 1 x H x W, relative inverse depth
        features = model.pretrained.get_intermediate_layers(
            net_input, [2, 5, 8, 11], return_class_token=True)
    forward_seconds = time.time() - started

    for handle in handles:
        handle.remove()

    # Pos embedding after bicubic interpolation (the graph adds it to cls+patches).
    #
    # prepare_tokens_with_masks unpacks an NCHW tensor as `B, nc, w, h`, so it
    # passes (w, h) = (height, width) to interpolate_pos_encoding. PyTorch's
    # F.interpolate reads scale_factor as (height_scale, width_scale), so the two
    # naming mistakes cancel and the result is the *natural* layout: rows are
    # y * patch_w + x over the real patch grid. Calling it as (net_w, net_h)
    # would transpose the grid, so mirror the reference argument order exactly.
    with torch.no_grad():
        patch_tokens = records["patch_embed"].flatten(2).transpose(1, 2)
        tokens = torch.cat((model.pretrained.cls_token.expand(1, -1, -1), patch_tokens), dim=1)
        pos_embed = model.pretrained.interpolate_pos_encoding(tokens, net_input.shape[2],
                                                              net_input.shape[3])

    files = {}
    files["010_input.npy"] = as_numpy(net_input)
    files["011_image_rgb.npy"] = rgb
    files["020_patch_embed.npy"] = as_numpy(patch_tokens)
    files["021_pos_embed.npy"] = as_numpy(pos_embed)
    for index in (2, 5, 8, 11):
        files[f"030_block_{index:02d}.npy"] = as_numpy(records[f"block_{index}"])
    for tap, (patch, cls) in enumerate(features):
        files[f"040_tap_patch_{tap:02d}.npy"] = as_numpy(patch)
        files[f"040_tap_cls_{tap:02d}.npy"] = as_numpy(cls)
    for index in range(4):
        files[f"050_layer_{index:02d}.npy"] = as_numpy(records[f"resize_{index}"])
    for index in range(4):
        files[f"060_path_{4 - index}.npy"] = as_numpy(records[f"path_{4 - index}"])
    files["061_conv1.npy"] = as_numpy(records["conv1"])
    files["070_depth_net.npy"] = as_numpy(depth[0])
    with torch.no_grad():
        full = F.interpolate(depth[:, None], (original_h, original_w), mode="bilinear",
                             align_corners=True)[0, 0]
    files["071_depth_full.npy"] = as_numpy(full)

    for name, array in files.items():
        # ascontiguousarray keeps the .npy files C order: numpy otherwise writes
        # Fortran order for transposed views, which the C++ reader does not take.
        if array.dtype == np.uint8:
            np.save(folder / name, np.ascontiguousarray(array))
        else:
            np.save(folder / name, np.ascontiguousarray(array.astype(np.float32)))

    meta = {
        "photo": image_path.name,
        "photo_id": photo_id,
        "source_size": [original_w, original_h],
        "net_size": [net_w, net_h],
        "patch_grid": [patch_w, patch_h],
        "input_size": input_size,
        "forward_seconds": forward_seconds,
        "depth_range": [float(depth.min()), float(depth.max())],
        "stages": {name: list(array.shape) for name, array in files.items()},
        "notes": "relative inverse depth (larger = closer); stages are NCHW unless the name says tokens",
    }
    (folder / "000_meta.json").write_text(json.dumps(meta, indent=2))
    return meta


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--weights", required=True, type=Path, help="Depth-Anything-V2-Small-hf safetensors")
    parser.add_argument("--repo", required=True, type=Path, help="ref/Depth-Anything-V2 checkout")
    parser.add_argument("--out", required=True, type=Path, help="golden/da2 output root")
    parser.add_argument("--photos", nargs="+", required=True, type=Path)
    parser.add_argument("--input-size", type=int, default=518)
    parser.add_argument("--skip-existing", action="store_true")
    parser.add_argument("--device", default="cpu", choices=["cpu", "mps", "cuda"])
    args = parser.parse_args()

    sys.path.insert(0, str(args.repo))
    from depth_anything_v2.dpt import DepthAnythingV2

    torch.manual_seed(0)
    model = DepthAnythingV2(encoder="vits", features=64, out_channels=[48, 96, 192, 384])
    state = hf_state_dict(read_safetensors(args.weights))
    loaded = model.load_state_dict(state, strict=True)
    if loaded.missing_keys or loaded.unexpected_keys:
        raise SystemExit(f"state dict mismatch: missing {loaded.missing_keys} "
                         f"unexpected {loaded.unexpected_keys}")
    model.eval().to(args.device)

    args.out.mkdir(parents=True, exist_ok=True)
    runs = []
    for photo in args.photos:
        folder = args.out / photo.stem
        if args.skip_existing and (folder / "071_depth_full.npy").exists():
            print(f"skip {photo.stem}")
            continue
        started = time.time()
        if args.device != "cpu":
            moved = model.to(args.device)
        else:
            moved = model
        meta = dump_photo(moved, photo, args.out, args.input_size)
        meta["seconds"] = time.time() - started
        runs.append(meta)
        print(f"{photo.stem}: {meta['net_size']} depth {meta['depth_range'][0]:.3f}.."
              f"{meta['depth_range'][1]:.3f} in {meta['seconds']:.1f} s")

    summary = {
        "weights": str(args.weights),
        "weights_sha256": sha256_file(args.weights),
        "weights_bytes": args.weights.stat().st_size,
        "repo": "github.com/DepthAnything/Depth-Anything-V2",
        "device": args.device,
        "platform": platform.platform(),
        "torch": torch.__version__,
        "peak_rss_gib": peak_rss_gib(),
        "runs": runs,
    }
    (args.out / "RUN.json").write_text(json.dumps(summary, indent=2))
    print(f"peak rss {peak_rss_gib():.2f} GiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
