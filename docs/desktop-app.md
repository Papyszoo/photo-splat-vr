# Desktop app mode (D1)

`splat_desktop --app <data dir>` is the whole photo-to-VR flow on one machine:

```
phone / curl ──► LAN page + PIN ──► inbox/ ──► InboxWatcher ──► JobQueue
                                                                   │
                       scene folder ◄── JobRunner + backend (C2) ◄──┘
                              │                  ▲
                              ▼                  │ GPU phase guard
                       scenes/ playlist ──► viewer (C6 renderer)
```

Conversion and viewing take turns on the GPU. The model is unloaded before the
renderer allocates, and the scene folder is the only hand-off between the two
phases. The upload page, the queue, the watcher and the phase guard are the
existing C9/C1/C5/C6 pieces; app mode only wires them together
(`platform/desktop/app_mode.{h,cpp}`) and adds the viewer loop
(`platform/desktop/main.cpp`).

## Running it

```sh
# build (any build directory name; build*/ is gitignored)
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-app --target splat_desktop

# one converted GGUF per backend you want to use, under <data dir>/models/
mkdir -p ~/splat-app/models
ln -s "$SPLAT_DATA_DIR/weights/dav2s-f16.gguf"          ~/splat-app/models/da2.gguf
ln -s "$SPLAT_DATA_DIR/weights/sharp_2572gikvuh-f16-v1.gguf" ~/splat-app/models/sharp.gguf

# window + LAN page
build-app/platform/desktop/splat_desktop --app ~/splat-app
```

The terminal and the window title both show the page and the PIN:

```
[app] upload page http://192.168.x.x:8090/   PIN 123456
[app] backend sharp  model /home/me/splat-app/models/sharp.gguf
```

Open that URL on a phone on the same LAN, type the PIN, pick a photo. The
conversion starts as soon as the upload lands in `inbox/`, and the finished
scene appears in the viewer (newest first).

The same upload with curl, if you prefer a shell:

```sh
# 1. login: the session cookie carries the PIN check
curl -s -c /tmp/splat.jar -d "pin=$PIN" http://127.0.0.1:8090/api/login
# 2. upload: multipart field "photo", JPEG or PNG
curl -s -b /tmp/splat.jar -F "photo=@photo.jpg" http://127.0.0.1:8090/api/upload
# 3. optional: watch the queue
curl -s -b /tmp/splat.jar http://127.0.0.1:8090/api/queue
```

### Options

| flag | meaning |
| --- | --- |
| `--app DIR` | data directory; `inbox/`, `scenes/`, `state/`, `models/` are created |
| `--backend auto\|sharp\|fallback` | `auto` (default) picks `sharp` when its GGUF is ready, else `fallback` |
| `--model FILE.gguf` | use this converted model instead of the one under `models/` |
| `--bind ADDR` | page address; default is the private LAN address. There is no implicit loopback: use `--bind 127.0.0.1` for a local-only page (see below) |
| `--port N` | page port (default 8090, `0` picks a free one) |
| `--headless` | no window: the page and the jobs still run |
| `--screenshot-dir DIR` | headless: render every new scene to `DIR/<job id>.png` |
| `--upload PHOTO` | upload PHOTO through the page (PIN login + multipart), repeatable |
| `--jobs N` | exit after N finished conversions |
| `--run-seconds N` | exit after N seconds |
| `--transition-log FILE` | where the phase log goes (default `DIR/state/transitions.log`) |
| `--cpu` | force the CPU ggml backend (no GPU needed) |
| `--size WxH`, `--stereo`, `--ipd MM`, `--scale S`, `--comfort-radius M`, `--fade-start F`, `--pose ...` | same as the single-scene viewer |

### Backend selection

1. `--model` decides on its own: `--backend` picks the backend, or with `auto`
   the file's GGUF metadata is checked against the SHARP pin first, then the
   DA2 pin.
2. `--backend sharp|fallback` without `--model`: the backend must be ready, or
   start fails with a message that says which file is missing.
3. `auto` without `--model`: `sharp` when `ModelManager` reports it ready
   (licence accepted, checkpoint verified, GGUF converted) **or** when
   `<data dir>/models/sharp.gguf` verifies against the pinned metadata;
   otherwise `fallback` with `models/da2.gguf` or the manager's own
   `state/models/da2.gguf`.

App mode never downloads. Put a converted GGUF in `models/` (or let
`tools/model_cli` convert one into `state/`) — the licence gate, download,
verification and conversion stay in the model manager's own flow.

### Page address

The page only ever listens on a private IPv4 address or on loopback; `0.0.0.0`,
a public address and host names are refused at start-up
(`refusing to bind …: the upload page is LAN-only`), so it cannot be exposed to
the internet by accident.

- Without `--bind`, app mode asks the host for a private LAN address
  (`core/ingest/lan_address.h`: 10/8, 172.16/12, 192.168/16). That is the
  address a phone on the same network uses.
- If the host has no such interface, `start()` fails and asks for `--bind
  127.0.0.1`. Loopback is never chosen silently: a page nobody can reach should
  be a decision, not a fallback.
- `--bind 127.0.0.1` serves the page on this machine only: use it with
  `--upload` or `curl` (macOS's application firewall drops incoming connections
  to unsigned binaries on the LAN interface, so this is also the way to demo on
  a laptop without signing the binary).

## What happens on a conversion (GPU turn-taking)

1. The upload lands in `inbox/`; `InboxWatcher` waits for a stable size, then
   enqueues the photo. `app.enqueue`.
2. The worker asks the viewer to leave: `app.inference.request`, then
   `GpuPhaseGuard::begin_inference()`, which blocks until no viewer holds the
   phase (`inference.begin`).
3. The viewer thread sees `release_requested()`/`work_pending()`, destroys the
   renderer (**its scene buffers, sort buffers and pipelines**), and calls
   `end_viewing()` (`viewer.end`, `app.viewer.released`). The window keeps
   presenting the neutral backdrop, so nothing reallocates during inference.
4. `JobRunner` loads the backend (`model.loaded`), runs it, writes the C5
   folder, then unloads and reports `model.unloaded` **before** it ends
   inference (`inference.end`). The guard refuses viewers while the model is
   resident, so the order is enforced, not just intended.
5. The viewer re-enters (`viewer.begin`) and allocates with the newest scene
   (`app.viewer.allocated`). New scenes join the playlist newest first, so the
   freshly converted one is on screen.

`[` / `]` browse the playlist, scroll or `+`/`-` change world scale, `R`
recentres, `P` pauses, `WASD` moves the head, `ESC` quits. Browsing reuses the
renderer's staging slot (and the background `ScenePreloader`), so the current
scene keeps rendering until the swap commits.

## The transition log

One line per event, appended and flushed immediately, so a crash still leaves
the history. Every line carries the same trailer:

```
2026-10-26T04:16:44.098Z inference.end phase=idle model=0 rss=105.70 MiB peak=168.39 MiB gpu=1.16 MiB/16.00 GiB
```

| field | meaning |
| --- | --- |
| `2026-10-26T04:02:53.607Z` | wall clock, UTC, milliseconds |
| `inference.end` | event (see the table below) |
| `phase=` | guard state after the event: `idle`, `inference` or `viewing` |
| `model=` | `1` while the backend weights are resident |
| `rss=` | this process's resident set right now |
| `peak=` | high-water resident set since start |
| `gpu=` | device-local heap usage / heap size (`VK_EXT_memory_budget`); `heap=… (usage unknown)` when the driver reports no usage, `n/a` without a device |

On the PS5 and on Apple silicon the GPU memory is part of `rss` (unified
memory), so the two numbers together are the honest picture; on a discrete GPU
`gpu=` is the one that matters.

Events:

| event | written by | meaning |
| --- | --- | --- |
| `app.start` | session | data dir and log path |
| `app.backend` | session | chosen backend, model file, why (`auto:` … or `--model`) |
| `app.backend.failed` | session | no usable backend; `start()` fails |
| `app.server.failed` | session | the page could not bind |
| `app.enqueue` / `app.enqueue.failed` | worker | photo picked up from the inbox |
| `app.inference.request` | worker | the viewer was asked to release the GPU |
| `inference.begin` / `inference.end` | phase guard | conversion owns / released the GPU |
| `model.loaded` / `model.unloaded` | phase guard | weights resident / released |
| `app.job.stage` | worker | backend progress (`stage=`, `fraction=`) |
| `app.job.done` | worker | job id, seconds, wall time, splat count, backend, folder |
| `app.job.failed` | worker | job id, state and the runner's message |
| `app.job.error` / `app.job.none` | worker | runner-level error, or a spurious empty run |
| `app.job.backend_override` | worker | a queued job asked for another backend than this session's |
| `app.playlist` | worker | the scene list changed (`scenes=`, `newest=`) |
| `app.inbox.archived` / `app.inbox.archive_failed` | worker | the converted photo was moved out of `inbox/` (or could not be) |
| `viewer.begin` / `viewer.end` | phase guard | a viewer took / released the GPU phase |
| `app.viewer.allocated` | viewer | renderer (re)created for a scene (`scene=`, `splats=`) |
| `app.viewer.released` | viewer | scene buffers freed before inference |
| `app.viewer.scene` | viewer | scene swapped through the staging slot (`commit_ms=`) |
| `app.viewer.recentre` | viewer | `R`: new anchor |
| `app.signal` | main loop | `SIGINT`/`SIGTERM` received; the session is about to stop in order |
| `guard.shutdown` | phase guard | the session is closing; always written by `stop()` |
| `app.stop` | session | uptime in seconds |

A run is correct when, for every conversion, a `viewer.end` precedes
`inference.begin`, `model.unloaded` precedes `inference.end`, and the next
`viewer.begin` comes after both. The guard writes each transition line before it
wakes the other side and while it still holds its own lock, and the trailer is
taken from that same state snapshot, so the file order is the real order — not a
re-read of a state another thread already moved on. `tests/app_mode_test.cpp`
and `tests/jobs_phase_test.cpp` assert exactly that from the log.

## Stopping it

`ESC`, the window's close button, `Ctrl-C` (`SIGINT`), `SIGTERM`, `--jobs` or
`--run-seconds` all end through the same path, in this order:

1. `app.signal signal=SIGINT` (only for a signal) is logged when the main loop
   notices the request.
2. `app.stop()`: the upload page stops, the guard is shut down (`guard.shutdown`,
   also when a conversion is running — a worker parked waiting for the GPU gives
   up), and the worker is joined, so a job that already owns the GPU still
   finishes, unloads its model and writes its scene folder.
3. The renderer, swapchain and Vulkan context are destroyed; only then does the
   process exit.

The signal handler itself does one lock-free atomic store: no allocation, no
logging, no Vulkan from a signal context. Without `--jobs`/`--run-seconds`, a
headless session runs until one of those signals arrives.

## Data directory

```
<data dir>/
  inbox/            uploaded photos (the watcher also scans this recursively)
  scenes/<job id>/  original.jpg, thumb.jpg, scene.psplat, meta.json
  state/queue.json  persisted queue (survives restarts, running jobs re-queue)
  state/ingested/   inbox photos after a successful conversion
  state/transitions.log
  state/models/     the model manager's converted GGUFs
  models/           GGUF drop folder: sharp.gguf, da2.gguf
```

## Tests

```sh
export SPLAT_DA2_GGUF=$SPLAT_DATA_DIR/weights/dav2s-f16.gguf
ctest --test-dir build-app --output-on-failure
build-app/tests/splat_tests -tc="app mode*"
```

`tests/app_mode_test.cpp` uploads a golden photo through the HTTP API with the
PIN, waits for the job, checks the scene folder and the playlist, and checks
the phase ordering in the transition log. A GPU-free "fake viewer" thread does
the same acquire/release dance as the desktop loop, so the ordering is tested
on hosts without a device; the window, the renderer and the screenshots are
covered by the manual run below. It also stops a session in the middle of a
conversion (`guard.shutdown`, `phase_guard_stopped()`) and checks that the page
can only be bound to a private address or an explicit loopback. The cases skip
when the DA2 GGUF or the golden photo is missing.

`tests/jobs_phase_test.cpp` hammers the guard with 200 back-to-back
inference/viewing alternations from two threads, stalls the observer on the
releasing transitions the way a log write does, and replays the recorded events
as a state machine: a `viewer.begin`/`inference.begin` recorded before the
matching end fails the test.

## Manual run (reproducible)

```sh
DATA=$SPLAT_DATA_DIR/runs/app/data
mkdir -p "$DATA/models"
ln -sf "$SPLAT_DATA_DIR/weights/dav2s-f16.gguf" "$DATA/models/da2.gguf"

build-app/platform/desktop/splat_desktop \
  --app "$DATA" --backend fallback --bind 127.0.0.1 \
  --headless --screenshot-dir "$SPLAT_DATA_DIR/runs/app" \
  --upload "$SPLAT_DATA_DIR/golden/immichvr/4.jpg" \
  --upload "$SPLAT_DATA_DIR/golden/immichvr/5.jpg" \
  --jobs 2 --run-seconds 300 --port 8099 \
  --transition-log "$SPLAT_DATA_DIR/runs/app/transitions.log"
```

That writes `transitions.log`, `run.log`, `job-000001.png` and
`job-000002.png` under `$SPLAT_DATA_DIR/runs/app/`. For the SHARP job, symlink
`sharp_2572gikvuh-f16-v1.gguf` as `models/sharp.gguf` in a second data
directory and drop `--backend fallback`.

Measured on this Mac (Apple M4, MoltenVK, fallback on the Vulkan backend):

| job | photo | backend | time | splats | GPU peak | after unload |
| --- | --- | --- | --- | --- | --- | --- |
| `job-000001` | `4.jpg` | fallback | 0.79 s | 499,217 | 533 MiB | 720 KiB |
| `job-000002` | `5.jpg` | fallback | 0.31 s | 498,041 | 533 MiB | 720 KiB |
| `job-000001` (second run) | `4.jpg` | sharp | 70.5 s | 1,179,648 | 4.22 GiB | 720 KiB |

Every job loads and unloads its backend, so `seconds=` includes the model load.
The fallback stays between 0.3 s and 0.8 s per photo depending on the page
cache and what else the machine is doing; SHARP measured 51 s and 70 s in two
runs here. The fallback's 533 MiB GPU peak is the depth network's activations
(the scene itself is ~35 MB fp16); SHARP holds 1.22 GiB of weights right after
`model.loaded` and peaks at 4.22 GiB in the Gaussian decoder. In both cases the
`model.unloaded` line shows the GPU back at the empty-device baseline
(720 KiB), which is what lets the renderer allocate.

## Limits

- One backend per session: `JobQueue` stores the backend name per job, but the
  runner gets one model path. A job queued by an earlier session with another
  backend is converted with the current one and logged as
  `app.job.backend_override`.
- App mode never downloads or converts checkpoints; that is C1's job
  (`tools/model_cli`).
- `--jobs`/`--run-seconds` are the scripted exits; an interactive run exits on
  `ESC`, the window's close button, `Ctrl-C` or `SIGTERM`. A job that is already
  running finishes before the process exits (the guard is shut down first, but
  that only keeps new phases out); a job still waiting for the GPU phase is
  recorded as interrupted instead.
- A converted photo is moved from `inbox/` to `state/ingested/`, so restarting
  the app does not convert it again. A failed job leaves its photo in the inbox,
  which makes it visible and retryable after the cause is fixed.
- The console build will drive the same session with the Hmd2 layer instead of
  the GLFW window; the viewer-side calls (`release_requested`,
  `try_begin_viewing`, `end_viewing`) are the only contract it needs.
