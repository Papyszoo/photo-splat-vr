# Single-photo -> 3D models worth considering next to SHARP

Read-only survey (2026-10-26). Nothing here was executed: every PS5 time is an
**estimate** from parameter/token counts against the measured SHARP (11.8 s,
655 M live params, 1536^2, 35-patch ViT-L x2) and DA2-Small (1.0 s, 24.8 M)
numbers. Every licence in the tables was verified against the repo `LICENSE`
and the HF model card on 2026-10-26 (R4-lic, `runs/r4_licences/REPORT.md`);
"weights licence" is the one that binds a shipped or converted GGUF.
Scope: one photo -> something a PSVR2 viewer can move +-10-30 cm through.

## A. Depth / geometry models (feed our own compositor)

| Model (size) | Code / weights licence (verified 2026-10-26) | Output | Published evidence (edges, metric) | Params / weights | Est. PS5 time | ggml port effort |
| --- | --- | --- | --- | --- | --- | --- |
| **SHARP** (baseline) | Apple sample-code licence / **apple-amlr (research only, no commercial product)** | 1.18 M Gaussians, 2 depth layers, metric | measured here: image parity 35.5 dB, 1.18 M splats, 7.2 GiB | 655 M live / 1.22 GiB fp16 | **11.8 s measured** | done |
| **DA2-Small** (baseline) | Apache-2.0 / Apache-2.0 *(verified 2026-10-26)* | relative depth (metric variants exist) | capture PSNR 18.3-28.2 dB, +-5 cm backdrop 0-2 %, but ramp/zebra at silhouettes | 24.8 M / 0.1 GB | **1.0 s measured** | done |
| **DA2-Large** | Apache-2.0 / **CC-BY-NC-4.0** *(verified 2026-10-26; Base is NC too, Metric-* cards carry no licence tag)* | relative depth | no gain over Small at the capture pose (measured) | 335 M / 1.3 GB | ~11 s | done (`exp/da2-large`) |
| **Depth Pro** | Apple sample-code licence / **apple-amlr (research only)** *(verified 2026-10-26, ungated)* | metric depth + focal, no splats | best boundary avg rank (1.50) in MoGe-2 Table 3; δ1 0.932 NYU, 0.386 ETH3D | ~0.47 B / 1.9 GB | ~6 s (0.5x) | trivial: it *is* SHARP's stage 1-2 |
| **DA3-Small / Base** | Apache-2.0 / **Apache-2.0** *(verified 2026-10-26)* | relative depth + ray map (camera) | no per-size monocular table; family beats DA2 mono (δ1 97.1/98.0/82.3/98.8/96.5 vs 94.6/97.9/77.2/86.5/95.2 KITTI/NYU/Sintel/ETH3D/DIODE) | 34 M / 0.14 GB; ~0.11 B / 0.54 GB | 0.3-1.0 s (0.03-0.08x) | low: DINOv2 ViT + DPT, same shapes as DA2 |
| **DA3-Mono-L / Metric-L** | Apache-2.0 / **Apache-2.0** *(verified 2026-10-26; the newer `DA3-LARGE-1.1` card says Apache-2.0 but the repo's model-zoo table says CC BY-NC 4.0 — treat as NC until confirmed)* | relative (Mono) or **metric** (Metric) depth | Metric: δ1 0.917 / AbsRel 0.104 ETH3D, 0.973 / 0.105 SUN-RGBD (best ETH3D, 2nd DIODE) | 0.35 B / 1.34 GB | 1-3 s @504, 3-6 s @1008 (0.1-0.3x) | low-medium: DINOv2 ViT-L + DualDPT + ray head; RoPE/attention layout to verify |
| **MoGe-2** ViT-S/B/L | MIT (DINOv2 parts Apache) / **MIT** *(verified 2026-10-26)* | **metric point map** + depth + normals + FOV, OpenCV frame | point Rel 8.19 / δ1 93.6 vs Depth Pro 13.7 / 81.9; boundary F1 iBims 17.9 (Depth Pro 14.3), Sintel 35.2 (41.6), HAMMER 5.42 (5.36) | 35 / 104 / 331 M; 0.14 / 0.42 / 1.31 GB | ~0.6 / 1.2 / 2.5 s @1024; ~1.2 / 2.5 / 5 s @1536 (0.1-0.4x) | low: ViT + DPT point head + small scale/FOV MLP; normals = finite differences (no new ops) |
| **MoGe-3** ViT-L/g | MIT / **MIT** *(verified 2026-10-26; ViT-L is also released; upstream is Linux-only — needs FlexGEMM/Triton)* | as MoGe-2, finer detail + refinement steps | paper claims "fine-detail" gains over MoGe-2; no third-party numbers yet | 1.25 B (ViT-g) / ~5 GB fp32; ViT-L 1.48 GB | 15-25 s (1.5-2x) | medium: adds iterative sparse-volumetric refinement |
| **UniDepth V2** | **CC-BY-NC-4.0** / no licence tag on the weights *(verified 2026-10-26)* | metric point map + depth + intrinsics, any camera | point Rel 10.1 / δ1 91.9; best NYU/KITTI metric (δ1 0.968/0.968) | 354 M / 1.4 GB | 2-4 s (0.2-0.3x) | medium: camera/spherical-embedding module |
| **Metric3D v2** | BSD-2-Clause / **none: no README, no LICENSE, no tag; repo README asks commercial users to contact the authors** *(verified 2026-10-26)* | metric depth + normals (needs focal/canonical camera) | δ1 0.971 NYU, 0.976 KITTI, 0.830 ETH3D | 0.3 B (L) / 1.4 B (g) | 2-4 s (L), 10-20 s (g) | medium-high: RAFT-style iterative decoder (correlation + GRU) |
| **Marigold / Lotus** | Apache-2.0 / **OpenRAIL++-M** (Marigold), Apache-2.0 (Lotus) *(verified 2026-10-26; OpenRAIL use restrictions are not GPL-compatible)* | affine-invariant relative depth, diffusion | sharp-looking but low-frequency; no metric; no NVS evidence | 865-868 M / ~3.5 GB | 10-50 s (1-50 steps; 1-4 step Lotus 4-8 s) | high: UNet + VAE + scheduler |

## B. Image -> 3D Gaussians / novel-view models (would replace the compositor)

| Model | Code / weights licence (verified 2026-10-26) | Output | Evidence at small baselines | Params / weights | Est. PS5 time | Port effort |
| --- | --- | --- | --- | --- | --- | --- |
| **Flash3D** | **no LICENSE file** on the code repo (issue #26 still open) / HF card now tags `mit` but ships no licence text — **unclear** | 1 depth layer + K offset Gaussian layers -> **fills occlusions** | RE10K PSNR 28.46 (5-frame), 25.94 (10-frame), 24.93 (+-30-frame); NYU 25.45, KITTI 21.96 | ~0.17 B saved + 0.35 B frozen UniDepth / 0.69 GB + 1.4 GB | 3-5 s (0.3-0.4x) | medium; **blocked by licence** and by UniDepth (CC-BY-NC) |
| **Splatter Image** | BSD-3-Clause / **MIT** (HF) *(verified 2026-10-26)* | 2 Gaussians per pixel, object-centric | RE10K-adapted 28.15/25.34/24.15 (same table); Objaverse/CO3D objects only | ~0.17 B / 0.68 GB per class | 1-2 s | low, but no scenes, no metric, unit-cube camera |
| **Gen3C** | Apache-2.0 / **NVIDIA Open Model License (commercial use allowed, but use restrictions + notice duties: not GPL-compatible)** *(verified 2026-10-26)* | generated novel-view video, 3D-cache conditioned -> fills disocclusion | CVPR'25 sparse-view NVS SOTA; per-frame diffusion, no small-baseline PSNR table | 7 B (Cosmos) + MoGe depth / 29 GB fp32 | minutes/frame, >11 GiB | not portable (7 B video DiT) |
| **2026 feed-forward 3DGS**: ZipSplat, YoNoSplat, QuerySplat, InfiniSplat v2 | ZipSplat Apache-2.0 / **CC-BY-NC-4.0 weights**; YoNoSplat MIT / MIT; UniSHARP CC-BY-NC-4.0 / no tag; QuerySplat, InfiniSplat v2 unverified *(ZipSplat/YoNoSplat/UniSHARP verified 2026-10-26)* | full 3DGS scene from images | DL3DV NVS; ZipSplat <1 s, YoNoSplat ICLR'26 | 0.3-1 B / 1-7 GB | 5-20 s | **need 2-50 views**, not one photo; several weight terms unclear/NC backbones (VGGT-Omega, DA3-Giant) |

## What the survey actually says

- **Nothing licence-clean matches SHARP.** SHARP's edge is not its depth (Depth
  Pro, its own encoder, is research-only too) but the learned 2-layer Gaussian
  decoder: per-splat scale/quaternion/opacity plus a second layer behind the
  first. Every permissive model here is geometry-only, so disocclusion still
  comes from our fill, which is exactly where the fallback loses to SHARP.
- **Flash3D is the only single-image model with learned occlusion fill** and it
  is unusable: no `LICENSE` (GitHub issue #26, open since 2026-05), the weights
  repo only carries a bare `license: mit` tag with no licence text, and it is
  built on UniDepth (CC-BY-NC). It is worth
  copying the *idea* (K offset layers along the ray, layer 0 pinned to depth) into
  our own compositor; the code/weights are not.
- **DA3 splits its licence by size**: Small/Base and the Mono/Metric-Large
  fine-tunes are Apache-2.0; Large/Giant/Nested (the only ones with the 3DGS
  head, and NVS is trained with 12 context views) are CC-BY-NC-4.0. So DA3 gives
  us Apache depth, not Apache Gaussians. One caveat (verified 2026-10-26): the
  HF card for `DA3-LARGE-1.1` says `apache-2.0` while the repo's model-zoo table
  says CC BY-NC 4.0 — unclear, do not bundle it.
- **Depth Pro is a dead end for licensing**: same Apple terms as SHARP, same
  encoder, no splats.
- **MoGe-2 is the only MIT-weights model with Depth-Pro-class boundaries**, and
  its metric point map removes the EXIF/focal guess (SHARP's 30 mm default is a
  scale assumption). Boundary F1 is slightly behind Depth Pro on Sintel/Spring,
  ahead on iBims/HAMMER; metric point error is far better (8.19 vs 13.7).
- **Test resolution beats backbone size for edges.** MoGe-2's Fig. 3b shows
  boundary sharpness rising with test resolution for DINOv2-based models; for us
  a 35-104 M backbone at 1536 (1 point per pixel, 1.18 M points, the same count
  as SHARP) is cheaper than ViT-L at 1024 by token/param arithmetic (our
  estimate), so the small models are worth testing first, not as a compromise.
- **The 2026 feed-forward 3DGS wave is multi-view** (2-50 photos). Feeding it one
  photo puts it off-distribution; none reports single-image numbers.
- **Non-starters**: UniDepth V2, Metric3D v2 weights (unclear), DA2-Large,
  UniSHARP (CC-BY-NC, SHARP-derived); Marigold/Lotus and Gen3C are 0.9-7 B
  diffusion models, 4-100x SHARP.

## Ranked recommendation (try these three, in order)

### 1. MoGe-2 ViT-B at 1536, ViT-L at 1024 as the quality reference (MIT, verified 2026-10-26)
Reason: best licence-clean geometry in the survey, metric, sharp edges, normals,
and the point map is already a splat layout (1 point per pixel). Normals let the
compositor orient discs instead of the current axis-aligned z-thin layers, which
is a plausible fix for the ramp/zebra artefacts (docs/fallback.md).
Cheapest test: a small torch harness (weights only, no upstream code) in the
style of `tools/golden/dump_golden.py`: run ViT-S/B/L on the five photos at 1024
and 1536, dump point map + normal + FOV; add a "point map" input path to
`FallbackReconstructor` (bypasses depth->unproject and the focal default); render
with `splat_desktop` at 0, +-5, +-10, +-30 cm and compare PSNR, pure-backdrop %
and the ramp count against SHARP and DA2-S. ~1.5 days, no ggml yet.
Gate: capture PSNR within 1 dB of DA2-S and 10 cm backdrop clearly below it,
or fewer ramp artefacts in the VR screenshots. If it passes, the port is a ViT +
DPT (we have both) and ~1 week.

### 2. DA3-Metric-Large (Apache-2.0, verified 2026-10-26)
Reason: the permissive metric model with the strongest published cross-domain
generalisation (ETH3D δ1 0.917 vs Depth Pro 0.386, SUN-RGBD 0.973), and it runs
at 504 px on the long side by default, so it is the cheapest metric option
(1-3 s). It is also the natural successor to DA2 in the shipping fallback if
SHARP ever has to go.
Cheapest test: reuse the existing ViT-L + DPT ggml path with a DA3 converter
(DualDPT, 1.34 GB fp32 -> 0.67 GB fp16), run at 504 and 1008 on the five photos,
compare capture PSNR and 10 cm backdrop against DA2-S, and check metric scale
against a tape-measured wall distance. ~2-3 days including the converter.
Risk: 504 res gives soft silhouettes; only 1008 makes it interesting, at 3-6 s.

### 3. DA3-Small (34 M, Apache-2.0 verified 2026-10-26, 0.14 GB)
Reason: the cheapest possible upgrade to the product's default backend - same
ViT-S + DPT family as DA2-Small, so it is a converter + one new checkpoint, not a
new backend. If DA3's better geometry survives at 34 M it replaces DA2-S at
roughly the same 1 s, and the fallback's licence story improves for free.
Cheapest test: one afternoon with the converter from #2 at 504/1008; gate on
capture PSNR and +-10 cm backdrop over the five photos. If it does not beat
DA2-S, drop DA3-Small and keep #2 only as the metric/scale path.

### Do not spend time on
Depth Pro (same licence, no splats); UniDepth V2 / Metric3D v2 / DA2-Large /
UniSHARP (NC or unclear weights); Marigold/Lotus (0.87 B diffusion, relative
depth only); Splatter Image (objects); Gen3C (7 B, host-only at best); Flash3D
until a licence appears; any 2026 feed-forward 3DGS that needs 2+ views.

## Sources
- SHARP: github.com/apple-aiml-research/ml-sharp (`LICENSE`, `LICENSE_MODEL`);
  `docs/research/sharp-reference.md`.
- DA2: github.com/DepthAnything/Depth-Anything-V2; HF `depth-anything/Depth-Anything-V2-Small-hf`
  (apache-2.0), `...-Large-hf` (cc-by-nc-4.0).
- Depth Pro: github.com/apple/ml-depth-pro (`LICENSE`); HF `apple/DepthPro`
  (`license: apple-amlr`).
- DA3: github.com/ByteDance-Seed/Depth-Anything-3 (Apache-2.0), model-zoo licence
  table, arXiv 2511.10647 (Tables 10, 11); HF `depth-anything/DA3-SMALL`,
  `DA3METRIC-LARGE`.
- MoGe-2/3: github.com/microsoft/MoGe (`LICENSE`: MIT + Apache DINOv2);
  HF `Ruicheng/moge-2-vitl*` (mit); arXiv 2507.02546 (Tables 2, 3).
- UniDepth V2: github.com/lpiccinelli-eth/UniDepth (`LICENSE`: CC-BY-NC-4.0);
  HF `lpiccinelli/unidepth-v2-vitl14`.
- Metric3D v2: github.com/YvanYin/Metric3D (`LICENSE`: BSD-2; README asks
  commercial users to contact the authors); HF `JUGGHM/Metric3D` (no licence tag).
- Marigold: github.com/prs-eth/Marigold (`LICENSE.txt` Apache-2.0,
  `LICENSE-MODEL.txt` OpenRAIL++-M); Lotus: github.com/EnVision-Research/Lotus
  (Apache-2.0), HF `jingheya/lotus-depth-g-v1-0` (apache-2.0).
- Flash3D: github.com/eldar/flash3d (no licence; issue #26), arXiv 2406.04343
  (Tables 1, 2); HF `einsafutdinov/flash3d` (no metadata).
- Splatter Image: github.com/szymanowiczs/splatter-image (BSD-3); HF
  `szymanowiczs/splatter-image-v1` (mit).
- Gen3C: github.com/nv-tlabs/GEN3C (Apache-2.0); HF `nvidia/GEN3C-Cosmos-7B`
  (NVIDIA Open Model License).
- Multi-view 2026: github.com/cvg/ZipSplat (Apache-2.0), github.com/cvg/YoNoSplat
  (MIT), github.com/Insta360-Research-Team/UniSHARP (CC-BY-NC-4.0).
