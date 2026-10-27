# Decisions and plan

This file records the product and technical decisions behind the code. The
specification is [`spec.md`](spec.md); known issues and follow-ups are in
[`BACKLOG.md`](BACKLOG.md).

## Project decisions

- Full spec is the goal, including running and debugging on a real console with
  live logs. The console comes from `$PS5_HOST` (LAN), firmware 13.60 assumed,
  PSVR2 attached. Deploy over elfldr (port 9021), logs over klog.
- The SHARP backend is an independent reimplementation of the published
  architecture; the user accepts Apple's `LICENSE_MODEL` for local research use
  and weights stay outside the repository.
- Licence: GPL-3.0-or-later, no weights, no addresses in the tree.
- **One app per install on the console** (2026-10-26, revised 2026-10-27):
  every test (benches, headset, viewer) installs into the same title ID and
  updates it in place, no new app per test. Two identities, selected by one
  build variable (`platform/ps5/title/identities.json`): dev `PPSA99202`
  ("Photo Splat VR Dev", the default) and release `PPSA99770` ("Photo Splat
  VR"). Both may be installed at once and **share no data**: the dev build
  keeps `/data/ps5-splat-vr` (all test material and models), the release build
  uses the user's private `/data/photo-splat-vr` and downloads its own models.
  `PPSA99202` is the id the dev build used before the Twiso collision; Twiso
  must not be on it (the uninstall script refuses anything but our two names).
  Agent scripts never launch `PPSA99770` and never touch
  `/data/photo-splat-vr` (AGENTS.md; enforced by `ps5-env.sh`).
- **Input** (2026-10-26): the VR viewer works with the DualSense (preferred) and
  with the PSVR2 Sense controllers, with the same mapping on both (Sense support
  follows HLA-PSVR2's scePad usage).
- **Models in releases** (2026-10-26): there is a release package with no model
  weights at all. Every model (SHARP after accepting Apple's licence, and the
  permissive candidates MoGe-2 / DA3, see
  [`research/single-image-3d-models.md`](research/single-image-3d-models.md)) is
  downloaded from a model screen in the app UI.
- **Browse UI** (2026-10-26): a UI to browse the console for photos, group
  processed splats (folders/albums) and pick one to display; investigation in
  [`research/browse-ui.md`](research/browse-ui.md).
- VR display path: study the HLA-PSVR2 port (Half-Life: Alyx on a jailbroken PS5
  with PSVR2) for Spike B step 3; findings in
  [`research/hla-psvr2.md`](research/hla-psvr2.md).

## Phases

- Phase 0: A) console probe + payload SDK + hello payload with live log;
  B) host toolchain + repo skeleton; C) SHARP reference, open questions, op
  inventory.
- Phase 1: desktop core — C5 scene store, C1 converter, C6 renderer (MoltenVK
  desktop), C4 fallback end to end. Spike A: SHARP on ggml (op audit, golden
  parity on desktop, then console timing). Spike B: RADV + Hmd2 buffer interop
  on console.
- Phase 2+: per spec, after both spike gates pass.

## Test data (outside the repo)

- golden/photo01..03: SHARP reference stage dumps + .ply (CPU fp32).
- golden/immichvr/: 1-3.jpg with SHARP .ply outputs, 4-5.jpg photos only. Use
  for renderer and end-to-end tests.
- Large artifacts live under `$SPLAT_DATA_DIR` (see [`../AGENTS.md`](../AGENTS.md)).

## SHARP only (2026-10-26)

No Depth Anything fallback in the product (tried in ImmichVR, results were poor;
only SHARP gives the intended result). The existing fallback code stays as a
contingency, e.g. if Apple objects to SHARP (DMCA), and as the DA2-Small
converter option. V2 Large was measured on branch `exp/da2-large` (~10.8x
Small's time, same capture-pose PSNR); Small is the one kept.

## First headset test of splat scenes (2026-10-26 evening)

- Works, 60 fps rendered (120 Hz), feels snappy; L1/R1 and the Sense controllers
  work.
- Wrong: head roll is not world-locked (rolling left makes the scene roll the
  opposite way); on yaw/pitch the scene moves a little with the head; no
  3D/parallax feeling. Edges blink when the head moves (aliasing).
- Wanted: manipulate the scene - grab with the Sense controllers (move/rotate,
  two hands scale); the DualSense uses sticks/triggers. A UI: the scene
  description (name + model) below the scene; DualSense and Sense controller
  models with tooltips about the controls; a button that hides the tooltips, and
  while they are hidden that button stays highlighted on the controller model.
  Font: font8x8 (public domain) approved.
- Testing aid: the app writes a head-pose + input log so a recorded video of a
  session can be matched to what the user did.

## Second headset test (2026-10-26 night)

- SHARP is "a few classes above" DA2; DA2 Small vs Large differ little: Large is
  not worth it (keep Small as the contingency). Compare more models in VR
  (permissive first: MoGe-2, DA3).
- Edges still blink (video recorded). Scaling is the most missed feature. A
  scene switch must NOT recentre: the world placement stays until the user
  recentres. Recentre = long press on Cross with a visible progress indicator (a
  short press does nothing).

## Third headset test (2026-10-26 late)

- Scale "sends us flying" (sometimes ridiculously far): scaling must change the
  scene in place, not move the viewer. Moving with the sticks is too slow. L1
  sometimes does nothing (e.g. on scene 13, going back failed several times).
- Models: SHARP is the best; MoGe-2 / DA3 / DA2 are mixed but "nice to have as an
  option in the application" (model screen,
  [`research/browse-ui.md`](research/browse-ui.md) 4.7).
- Model choice: SHARP is the recommended default; the models screen describes
  each model and lets the user pick.

## UI v1 decisions (2026-10-26 late)

- Full app UI: download models (PS5 downloads directly, licence accepted on
  screen, SHARP described as best quality), browse photos, convert with the
  selected model inside the app, browse scenes, organise them in folders
  (album = folder).
- Photo sources: internal storage (inbox / upload page from a phone or PC, and
  the PS5 screenshots through a small helper payload, since the title cannot see
  them) + USB.
- Folder names: the PS5 system keyboard if it works in VR, our own panel
  keyboard as fallback; new folders get a date name.
- Also queued: Sense grab (move/rotate, two-hand scale), DualSense and Sense
  controller models with control tooltips and a hide-tooltips button that stays
  highlighted while tooltips are off.
- Approvals (2026-10-27): HDR .jxr captures show as unsupported (switch
  screenshots to SDR); Options opens the menu, Quit is an optional menu item
  (the app also closes like any PS5 game); new console folders
  `<dataDir>/{shots,helper,state}` (dev: `/data/ps5-splat-vr`); the screenshot helper; mbedTLS only
  as fallback if sceHttp fails from a title; an unattended test may download
  SHARP (2.6 GB). The user must not have to install anything extra: the helper
  ships inside the app package and the app starts it itself; without a payload
  loader only Captures is unavailable.

## Fourth headset test (2026-10-27 night)

- Scale does nothing visible (world scale about the eye only changes stereo); it
  must visibly scale the scene without moving the viewer far. Sticks are
  inverted: stick forward/up/right must move the viewer forward/up/turn right.
  Background black. On the first start the scene sits to the right until a
  recentre. TV mirror works; L1/R1 fine.

## Headset test 5 feedback (2026-10-27) and decisions

- Scale (L2/R2) still looks like moving forward/back. Give a toggle to test both
  pivots: scene front (nearest surface ahead) and scene centre; research what is
  perceptually correct for scaling from the viewer's point of view
  ([`research/vr-scale.md`](research/vr-scale.md)).
- The VR menu panel skews when the head moves: fix.
- Movement too slow: Settings gets Move speed and Turn speed, separate. No snap
  turning (the user makes small adjustments). Turning is a little abrupt and
  stuttery: make it smoother.
- Aliasing much better; ImmichVR's splat renderer may have looked a bit better:
  compare.
- TV mode: the VR person keeps the VR menu in front of them. The TV never shows
  the VR menu (it would occlude the view). The TV person gets a separate TV-only
  menu (DualSense) fixed on the TV screen, so the headset person need not hold
  still for it; the TV shows only one menu.

## Headset test 6 feedback (2026-10-27) and decisions

- Scaling still reads as moving, also with the centre pivot. The user wants a
  maquette: shrinking puts the scene on a virtual table in front ("like it was
  on my table"), and L2/R2 then shrink or grow it in place there.
- A PS5-capture conversion failed with both models: investigate on the console.
- Keyboard: QWERTY, not ABC; holding the stick or d-pad must repeat and speed up.
  Prefer the PS5 system keyboard (IME dialog) when it works in TV and VR; the
  panel keyboard stays as the fallback.
- AA is good on the splats, but the menu panel and the scene label flicker a
  lot: they need minification filtering (mipmaps/anisotropy) and the label
  should use the smooth font.

## User decisions (2026-10-27, late)

- v0.1 release criterion: converting photos works on the console and the scenes
  display. TV-only menu, system keyboard and 3D controller models are not
  required for v0.1.
- Scene scale across models: SHARP keeps its real-world scale; DA2-Small scenes
  are scaled at conversion to a typical real-world size close to SHARP's
  (2026-10-27).
- More conversion models: research after v0.1 (quality, licence, PS5 speed and
  memory), then a short list for the user to pick from.

## v0.1 (2026-10-27)

- Released as **Photo Splat VR**: convert photos on the console and view them on
  PSVR2. DA2-Small ships as is, with approximate scene size.
