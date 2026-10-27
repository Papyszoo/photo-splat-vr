#!/usr/bin/env python3
"""Dump SHARP reference intermediate tensors ("golden") for one or more photos.

Hooks every stage of the inference forward pass that the C3 ggml port has to
reproduce, and writes the stage inputs/outputs as ``.npy`` plus the reference
``.ply``.  This is the parity oracle for ``tools/parity``: the port must match
these files (fp32: 1e-3 max relative error per stage, fp16: cosine >= 0.999).

Stages (spec C3 numbering, spec `docs/spec.md` C3):

    preprocess -> encoder -> depth decoder -> depth adjustment -> gaussian
    decoder -> composer -> unprojection

The depth-adjustment U-Net exists in the checkpoint but is *bypassed* at
inference (see ``docs/research/sharp-reference.md``); the dump records its
identity mapping for the record.

Requires the ml-sharp reference checkout and a venv with torch, timm, pillow,
pillow-heif, plyfile (``gsplat`` is only needed for rendering, not here):

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}
    V="$DATA"/ref/venv-sharp/bin
    python3.12 -m venv "$DATA"/ref/venv-sharp
    $V/pip install torch torchvision timm pillow pillow-heif \
        plyfile click imageio numpy scipy matplotlib
    $V/pip install -e "$DATA"/ref/ml-sharp --no-deps

Example:

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}   # the repo parent by default
    "$DATA"/ref/venv-sharp/bin/python tools/golden/dump_golden.py \
        --checkpoint "$DATA"/weights/sharp_2572gikvuh.pt \
        --out "$DATA"/golden \
        --photos "$DATA"/golden/inputs/*.jpg \
        --device mps

Output layout (one folder per photo, id = input file stem):

    <out>/<photo_id>/000_meta.json
    <out>/<photo_id>/010_image_uint8.npy        HxWx3 uint8, as loaded (EXIF-rotated)
    <out>/<photo_id>/011_preprocess_out.npy     1x3x1536x1536 f32, [0,1], align_corners=True
    <out>/<photo_id>/020_encoder_in.npy         1x3x1536x1536 f32, [-1,1]
    <out>/<photo_id>/021_encoder_out_{00..04}.npy
    <out>/<photo_id>/030_depth_decoder_out.npy  1x256x768x768 f32
    <out>/<photo_id>/031_depth_head_out.npy     1x2x1536x1536 f32 (disparity)
    <out>/<photo_id>/040_depth_align_out.npy    1x2x1536x1536 f32 (= in, identity)
    <out>/<photo_id>/041_monodepth.npy          1x2x1536x1536 f32 (metric, normalised)
    <out>/<photo_id>/050_gaussian_decoder_in.npy 1x5x1536x1536 f32
    <out>/<photo_id>/051_init_base_*.npy        gaussian base values
    <out>/<photo_id>/060_gaussian_decoder_out_{texture,geometry}.npy 1x32x768x768
    <out>/<photo_id>/070_delta_values.npy       1x14x2x768x768 f32
    <out>/<photo_id>/080_composer_{mean,singular,quaternion,color,opacity}.npy
    <out>/<photo_id>/090_unproject_*.npy        metric Gaussians (post unprojection)
    <out>/<photo_id>/reference.ply              3DGS ply, exactly as `sharp predict`

Nothing here is redistributable as a dataset: the folder stays outside the repo.
"""

from __future__ import annotations

import argparse
import json
import platform
import resource
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F

from sharp.models import PredictorParams, RGBGaussianPredictor, create_predictor
from sharp.utils import io as io_utils
from sharp.utils.gaussians import save_ply, unproject_gaussians

INTERNAL_SHAPE = (1536, 1536)


def peak_rss_gib() -> float:
    """Peak resident set size of this process, GiB (macOS reports bytes)."""
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return rss / 2**30


def _sha256(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def as_numpy(tensor: torch.Tensor) -> np.ndarray:
    return tensor.detach().to("cpu", dtype=torch.float32).numpy()


class Recorder:
    """Forward-hook recorder: keeps args/kwargs/output of named submodules."""

    def __init__(self) -> None:
        self.records: dict[str, tuple[tuple, dict, object]] = {}
        self.handles = []

    def attach(self, model: torch.nn.Module, name: str, module: torch.nn.Module) -> None:
        def hook(mod, args, kwargs, output):
            self.records[name] = (args, kwargs, output)

        self.handles.append(module.register_forward_hook(hook, with_kwargs=True))

    def detach(self) -> None:
        for handle in self.handles:
            handle.remove()
        self.handles.clear()


def dump_photo(
    model: RGBGaussianPredictor,
    image_path: Path,
    out_root: Path,
    device: torch.device,
    rec: Recorder,
) -> dict:
    """Run one photo through the model and write every stage."""
    photo_id = image_path.stem
    outdir = out_root / photo_id
    outdir.mkdir(parents=True, exist_ok=True)

    image, _icc, f_px = io_utils.load_rgb(image_path)
    height, width = image.shape[:2]
    np.save(outdir / "010_image_uint8.npy", np.ascontiguousarray(image))

    # --- stage 1: preprocess (sharp/cli/predict.py:169-178) ---
    image_pt = torch.from_numpy(image.copy()).float().to(device).permute(2, 0, 1) / 255.0
    disparity_factor = torch.tensor([f_px / width]).float().to(device)
    image_resized = F.interpolate(
        image_pt[None],
        size=(INTERNAL_SHAPE[1], INTERNAL_SHAPE[0]),
        mode="bilinear",
        align_corners=True,
    )
    np.save(outdir / "011_preprocess_out.npy", as_numpy(image_resized))

    rec.records.clear()
    t0 = time.perf_counter()
    with torch.no_grad():
        model(image_resized, disparity_factor)
    elapsed = time.perf_counter() - t0

    r = rec.records

    # --- stage 2: encoder (SlidingPyramidNetwork, Depth Pro dual ViT-L) ---
    enc_args = r["encoder"][0]
    np.save(outdir / "020_encoder_in.npy", as_numpy(enc_args[0]))
    encodings = r["encoder"][2]
    for i, enc in enumerate(encodings):
        np.save(outdir / f"021_encoder_out_{i:02d}.npy", as_numpy(enc))

    # --- stage 3: depth decoder (DPT, then the disparity head) ---
    np.save(outdir / "030_depth_decoder_out.npy", as_numpy(r["depth_decoder"][2]))
    disparity = r["depth_head"][2]
    np.save(outdir / "031_depth_head_out.npy", as_numpy(disparity))

    # --- stage 4: depth adjustment (bypassed at inference: depth is None) ---
    depth_out, align_map = r["depth_alignment"][2]
    np.save(outdir / "040_depth_align_out.npy", as_numpy(depth_out))
    np.save(outdir / "040_depth_align_map.npy", as_numpy(align_map))
    np.save(outdir / "041_monodepth.npy", as_numpy(depth_out))

    # --- stage 5: Gaussian decoder ---
    init_kw = r["initializer"][1]
    if init_kw:
        raise RuntimeError(f"unexpected initializer kwargs: {list(init_kw)}")
    np.save(outdir / "050_gaussian_decoder_in.npy",
            as_numpy(r["initializer"][0][0]))
    base = r["initializer"][2].gaussian_base_values
    np.save(outdir / "051_init_base_mean_x.npy", as_numpy(base.mean_x_ndc))
    np.save(outdir / "051_init_base_mean_y.npy", as_numpy(base.mean_y_ndc))
    np.save(outdir / "051_init_base_inv_z.npy", as_numpy(base.mean_inverse_z_ndc))
    np.save(outdir / "051_init_base_scale.npy", as_numpy(base.scales))
    np.save(outdir / "051_init_base_quaternion.npy", as_numpy(base.quaternions))
    np.save(outdir / "051_init_base_color.npy", as_numpy(base.colors))
    np.save(outdir / "051_init_base_opacity.npy", as_numpy(base.opacities))
    np.save(outdir / "051_init_global_scale.npy",
            as_numpy(r["initializer"][2].global_scale))

    feats = r["gaussian_decoder"][2]
    np.save(outdir / "060_gaussian_decoder_out_texture.npy",
            as_numpy(feats.texture_features))
    np.save(outdir / "060_gaussian_decoder_out_geometry.npy",
            as_numpy(feats.geometry_features))

    delta = r["prediction_head"][2]
    np.save(outdir / "070_delta_values.npy", as_numpy(delta))

    # --- stage 6: composer ---
    g_ndc = r["gaussian_composer"][2]
    np.save(outdir / "080_composer_mean.npy", as_numpy(g_ndc.mean_vectors))
    np.save(outdir / "080_composer_singular.npy", as_numpy(g_ndc.singular_values))
    np.save(outdir / "080_composer_quaternion.npy", as_numpy(g_ndc.quaternions))
    np.save(outdir / "080_composer_color.npy", as_numpy(g_ndc.colors))
    np.save(outdir / "080_composer_opacity.npy", as_numpy(g_ndc.opacities))

    # --- postprocess: unprojection to metric space (predict.py:185-206) ---
    intrinsics = (
        torch.tensor(
            [
                [f_px, 0, width / 2, 0],
                [0, f_px, height / 2, 0],
                [0, 0, 1, 0],
                [0, 0, 0, 1],
            ]
        )
        .float()
        .to(device)
    )
    intrinsics_resized = intrinsics.clone()
    intrinsics_resized[0] *= INTERNAL_SHAPE[0] / width
    intrinsics_resized[1] *= INTERNAL_SHAPE[1] / height
    gaussians = unproject_gaussians(
        g_ndc, torch.eye(4).to(device), intrinsics_resized, INTERNAL_SHAPE
    )
    np.save(outdir / "090_unproject_mean.npy", as_numpy(gaussians.mean_vectors))
    np.save(outdir / "090_unproject_singular.npy", as_numpy(gaussians.singular_values))
    np.save(outdir / "090_unproject_quaternion.npy", as_numpy(gaussians.quaternions))
    np.save(outdir / "090_unproject_color.npy", as_numpy(gaussians.colors))
    np.save(outdir / "090_unproject_opacity.npy", as_numpy(gaussians.opacities))
    np.save(outdir / "090_intrinsics_resized.npy", as_numpy(intrinsics_resized))

    save_ply(gaussians, f_px, (height, width), outdir / "reference.ply")

    num_gaussians = int(gaussians.mean_vectors.shape[1])
    meta = {
        "photo_id": photo_id,
        "source_image": str(image_path.resolve()),
        "image_size": [int(width), int(height)],
        "internal_shape": list(INTERNAL_SHAPE),
        "focal_length_px": float(f_px),
        "focal_length_mm_35mm": round(
            f_px * (36**2 + 24**2) ** 0.5 / (width**2 + height**2) ** 0.5, 4
        ),
        "disparity_factor": float(disparity_factor.item()),
        "device": str(device),
        "dtype": "float32",
        "forward_wall_time_s": round(elapsed, 3),
        "num_gaussians": num_gaussians,
        "num_layers": 2,
        "gaussians_per_layer": num_gaussians // 2,
        "sh_degree": 0,
        "sh_coeffs": 3,
        "ply_color_space": "sRGB",
        "composer_color_space": "linearRGB",
        "stage_shapes": {
            "encoder_out": [list(e.shape) for e in encodings],
            "depth_decoder_out": list(r["depth_decoder"][2].shape),
            "depth_head_out": list(disparity.shape),
            "depth_align": "identity (scale_map_estimator not run at inference)",
            "gaussian_decoder_out_texture": list(feats.texture_features.shape),
            "delta_values": list(delta.shape),
            "composer_mean": list(g_ndc.mean_vectors.shape),
            "composer_singular": list(g_ndc.singular_values.shape),
            "composer_quaternion": list(g_ndc.quaternions.shape),
            "composer_color": list(g_ndc.colors.shape),
            "composer_opacity": list(g_ndc.opacities.shape),
        },
        "attribute_ranges": {
            "mean_z_m": [float(gaussians.mean_vectors[..., 2].min()),
                         float(gaussians.mean_vectors[..., 2].max())],
            "singular_min": float(gaussians.singular_values.min()),
            "singular_max": float(gaussians.singular_values.max()),
            "opacity_min": float(gaussians.opacities.min()),
            "opacity_max": float(gaussians.opacities.max()),
            "color_min": float(gaussians.colors.min()),
            "color_max": float(gaussians.colors.max()),
        },
        "peak_rss_gib": round(peak_rss_gib(), 2),
        "torch": torch.__version__,
        "python": platform.python_version(),
        "platform": platform.platform(),
    }
    (outdir / "000_meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="golden root dir")
    parser.add_argument("--photos", required=True, type=Path, nargs="+")
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "mps", "cuda"])
    parser.add_argument("--skip-existing", action="store_true",
                        help="keep folders that already hold a reference.ply")
    args = parser.parse_args()

    if args.device == "auto":
        device = torch.device(
            "cuda" if torch.cuda.is_available()
            else "mps" if torch.mps.is_available()
            else "cpu"
        )
    else:
        device = torch.device(args.device)
    print(f"device: {device}")

    t0 = time.perf_counter()
    state_dict = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    print(f"checkpoint loaded in {time.perf_counter() - t0:.1f}s, "
          f"{sum(v.numel() for v in state_dict.values()):,} params")

    model = create_predictor(PredictorParams())
    model.load_state_dict(state_dict)
    model.eval()
    model.to(device)
    del state_dict

    rec = Recorder()
    rec.attach(model, "encoder", model.monodepth_model.monodepth_predictor.encoder)
    rec.attach(model, "depth_decoder", model.monodepth_model.monodepth_predictor.decoder)
    rec.attach(model, "depth_head", model.monodepth_model.monodepth_predictor.head)
    rec.attach(model, "depth_alignment", model.depth_alignment)
    rec.attach(model, "initializer", model.init_model)
    rec.attach(model, "gaussian_decoder", model.feature_model)
    rec.attach(model, "prediction_head", model.prediction_head)
    rec.attach(model, "gaussian_composer", model.gaussian_composer)

    results = []
    for photo in args.photos:
        if args.skip_existing and (args.out / photo.stem / "reference.ply").exists():
            print(f"skip {photo.stem} (exists)")
            continue
        print(f"== {photo}")
        meta = dump_photo(model, photo, args.out, device, rec)
        print(f"   {meta['num_gaussians']} gaussians, {meta['forward_wall_time_s']}s "
              f"forward, peak RSS {meta['peak_rss_gib']} GiB")
        results.append(meta)

    rec.detach()
    run_path = args.out / "RUN.json"
    previous = {}
    if run_path.exists():
        try:
            previous = json.loads(run_path.read_text()).get("photos_detail", {})
        except json.JSONDecodeError:
            previous = {}
    for meta in results:
        previous[meta["photo_id"]] = {
            "device": meta["device"],
            "num_gaussians": meta["num_gaussians"],
            "forward_wall_time_s": meta["forward_wall_time_s"],
            "peak_rss_gib": meta["peak_rss_gib"],
            "focal_length_px": round(meta["focal_length_px"], 3),
            "image_size": meta["image_size"],
        }
    run_path.write_text(
        json.dumps(
            {
                "checkpoint": str(args.checkpoint.resolve()),
                "checkpoint_sha256": _sha256(args.checkpoint),
                "device": str(device),
                "torch": torch.__version__,
                "python": platform.python_version(),
                "platform": platform.platform(),
                "notes": (
                    "CPU fp32 is canonical and bit-reproducible; MPS dumps of the "
                    "same photo differ by up to 1.8e-3 relative per element "
                    "(see docs/research/sharp-reference.md)."
                ),
                "photos": list(previous),
                "photos_detail": previous,
            },
            indent=2,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
