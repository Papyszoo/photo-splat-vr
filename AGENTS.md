# AGENTS.md — ps5-splat-vr

Read `docs/spec.md` (the technical spec) and `docs/PLAN.md` (current decisions) before working.

## Rules for every agent
- Stay inside the allowed area your brief names. Other agents work in this repo at the same time.
- No commits, no `git reset/checkout/stash`. The orchestrator commits.
- Run every command in the foreground and wait; never background a step.
- This repo becomes public later (GPL-3.0). Never commit: model weights (.pt, .gguf), converted files, golden tensors, the console IP, secrets. The console host comes from `PS5_HOST` (see gitignored `.env.local`).
- Large/external material lives outside the repo, under the data dir `$SPLAT_DATA_DIR` (default: the repo's parent directory):
  - `$SPLAT_DATA_DIR/ref/` — cloned reference repos (ml-sharp, PS5VR, PS5LM, vkgs, ...)
  - `$SPLAT_DATA_DIR/weights/` — downloaded checkpoints
  - `$SPLAT_DATA_DIR/golden/` — golden tensors and reference .ply files
  - `$SPLAT_DATA_DIR/runs/` — agent logs and scratch
- Never write absolute personal paths (/Volumes/..., /Users/...) into committed files: code, tests, scripts and docs resolve data via `SPLAT_DATA_DIR` (tests skip when data is absent). Console settings (`PS5_HOST`, ports) come from the environment or the gitignored `.env.local`.
- **Agents never launch `PPSA99770` and never touch `/data/photo-splat-vr`.** The console carries two installs (`platform/ps5/title/identities.json`): dev `PPSA99202` (data `/data/ps5-splat-vr`, the only one agent scripts may run) and release `PPSA99770`, whose data folder is the user's private data. No script in this repo may read, list, write or delete under `/data/photo-splat-vr`, and none may launch the release title (`ui-test.sh`, `run*.sh`, `push-data.sh`, `convert-inbox.sh`, `spike.sh`, `title.sh launch/stop/put`, ...). `ps5-env.sh` enforces both before a request is built (`ps5_agent_guard_identity`, `ps5_guard_remote_path`; offline test `platform/ps5/scripts/guard-test.sh`). `title.sh install/status` may install the `PPSA99770` package files under `/data/homebrew/PPSA99770`.
- Host: macOS arm64, 16 GB RAM. Never run a Docker build and the PyTorch reference at the same time. Cap PyTorch runs to one process.
- Keep code minimal and to the spec. No speculative features. Findings outside your task go to `docs/BACKLOG.md` as one line each.
- C++17 or later, CMake + Ninja. Tests use doctest (single header in third_party/).
