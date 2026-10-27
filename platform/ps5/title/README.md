# platform/ps5/title — the homebrew title package

The deliverable of the spec's goal 3, "ships as a homebrew title folder that
installs like other PS5 homebrew": a folder under `/data/homebrew/<TITLE_ID>`
that ShadowMountPlus registers and the home screen starts. Not a payload.
The research behind it is `docs/research/spike-b-memory.md`; this directory is
the build side of it.

Nothing here is installed on the console yet: the console was off for this
task, so the package is built and statically checked only (`docs/console-runbook.md`
section 1 is the gate that runs it).

## What a title gets that a payload cannot

Direct memory is a property of the *title process*: the system gives a title the
application pool (11.4-12 GiB), and RADV's PS5 winsys turns exactly that number
into its one device heap. A payload has budget 0, so `sceKernelGetDirectMemorySize()`
returns 0 and RADV creates a device with **no heaps** — every allocation then
fails. That is the session-1 result, not a driver bug
(`docs/research/spike-b-memory.md` sections 1-2).

## The package

Every program in this tree is packaged as the same title, `PPSA99202` for the
dev build (see "Two installs" below); the numbers are vk_hello's, the smallest
of them:

```
build/ps5/title/PPSA99202/
  eboot.bin               signed fSELF, magic 0x1D3D154F   ~24.5 MiB
  sce_module/libc.prx     C-library companion module       1.23 MiB
  sce_sys/param.json      generated: titleId, contentId, amm/kernel
  sce_sys/icon0.png       512x512 generated placeholder    3.8 KiB
```

Built today (2026-10-26, `make -C platform/ps5/title`):

| File | Bytes | Notes |
| --- | --- | --- |
| `eboot.bin` | 25,729,767 | `ps5-native-tool self --inspect`: signed, plaintext, 12 segments, integrity valid |
| `sce_module/libc.prx` | 1,284,674 | SHA-256 `e6ff45d1…0036`, the published artifact's hash, verified at build time |
| `sce_sys/icon0.png` | 3,911 | 512x512 RGB PNG from `gen-icon.py` |
| `sce_sys/param.json` | 1,064 | generated from `param.json` by `identity.py` (the dev identity below) |
| whole folder | 27,025,408 | |

The signed `eboot.bin` names six system modules (converted to NIDs):
`libSceAgc.prx`, `libSceAgcDriver.prx`, `libSceLibcInternal.prx`,
`libSceSystemService.prx`, `libSceVideoOut.prx`, `libkernel.prx`. It exports no
dynamic symbols (the title converter refuses exports; every bound libc name is
made local by a generated version script).

## Two installs: dev `PPSA99202`, release `PPSA99770`

User decision (2026-10-26, revised 2026-10-27; `docs/PLAN.md` "One app per
install on the console"): every test installs into the same title of its
identity and updates it in place, no new app per test. The identity is chosen
once, at build time, from `identities.json` in this directory — the only place
in the tree where a title id, concept id, content id, title name or console data
folder is written:

```sh
make -C platform/ps5/viewer                 # dev build (default) -> PPSA99202
make -C platform/ps5/viewer RELEASE=1       # release build       -> PPSA99770
make -C platform/ps5/viewer TITLE_ID=PPSA99770   # the same, by id
```

| Identity | titleId | conceptId | contentId (36 chars) | titleName | dataDir |
| --- | --- | --- | --- | --- | --- |
| dev (default) | `PPSA99202` | `99202` | `IV0000-PPSA99202_00-PHOTOSPLATVRDEV0` | Photo Splat VR Dev | `/data/ps5-splat-vr` |
| release | `PPSA99770` | `99770` | `IV0000-PPSA99770_00-PHOTOSPLATVR0001` | Photo Splat VR | `/data/photo-splat-vr` |

`identity.py` reads the table and writes the package's `sce_sys/param.json`
from `param.json` here, which is the identity-free template: icon, VR attributes
and AMM/kernel sizes, no ids and no name. `build-title.mk` takes the package
directory (`build/ps5/title/<TITLE_ID>`), the generated param.json, the
`SPLAT_TITLE_ID` string a title logs and the data folder the title and its
helper write (`SPLAT_DATA_ROOT`) from the same entry; the scripts
(`ps5_title_id`, `ps5_title_data_dir` in `ps5-env.sh`) read the same table, so
no second copy exists. `identity.py` refuses an id that is not in the table, a
content id that does not carry it, and two identities that share a data folder
rather than producing a package that would write another install's data.

Either identity packages every program — vk_hello (`platform/ps5/title/`), the
bench title (`platform/ps5/bench/`), vr_hello (`platform/ps5/vr_hello/`) and the
viewer (`platform/ps5/viewer/`) — as `build/ps5/title/<TITLE_ID>/`. One tile on
the home screen per identity, one `/data/homebrew/<TITLE_ID>/`, and each install
replaces that eboot in place. Both tiles may be installed at once.

**The two installs share nothing.** The dev build keeps `/data/ps5-splat-vr`
(all test scenes, models and logs); the release build uses the user's private
`/data/photo-splat-vr` and downloads its own models. The viewer and its payload
compile the folder in from the identity, so the helper's mailbox, pid lock and
inbox are per identity as well: a dev helper and a release helper never share a
lock or a mailbox, and neither can answer or copy for the other install. No
script in this repo may read,
list, write or delete under `/data/photo-splat-vr`, and no agent script may
launch `PPSA99770` (AGENTS.md): `ps5-env.sh` refuses both before a request,
`title.sh install/status` remain allowed, and `title-uninstall.sh --data
PPSA99770` is refused.

The identity is the viewer's, because the viewer is the app:

- `titleName` "Photo Splat VR Dev" / "Photo Splat VR", the ids above, and the
  generated icon;
- `attribute` `0x62000000` and `attribute3` `0x4080c40` (PS VR2 app | VR-only |
  120 Hz | hand tracking). They are what makes Hmd2 hand a frame to a title at
  all (`docs/research/hla-psvr2.md` section 4) and are harmless for a program
  that never opens the headset, so the same package serves the VR tests (viewer
  VR mode, vr_hello) and the non-VR ones (vk_hello, benches, viewer `--mode tv`).

Why the ids are safe: `PPSA99xxx` is the homebrew range and a `PPSA*` id gets
ShadowMountPlus's writable `/data` nullfs mount; neither id is used by the
projects in the research — PS5LM `PPSA99575`, PS5LM-Plus `PPSA99581`, PS5VR
`PPSA99177`, PS5_Vulkan `PPSA99014/99015/99170`, its runner `PPSA99988` — and a
collision would reuse that title's app.db entry and sandbox, so `title.sh`
refuses all of those. Both were checked free in the public catalogues
(`runs/release/PACKAGE-ID-verify.md`). `PPSA99202` collides with the Twiso
homebrew app only if Twiso is installed on it; the uninstall proof accepts
exactly "Photo Splat VR Dev" and the earlier "PS5 Splat VR Viewer", so Twiso is
refused.

To change or add an identity: edit `identities.json` (name, titleId, conceptId,
36-character contentId, titleName, dataDir, agentsMayRun) and rebuild;
`identity.py` validates every field, so a 15-character suffix like the old
`…VIEW1`, an empty title name or a shared data folder is refused at build time.
Add the new id to the reserved list in `platform/ps5/scripts/title.sh` if it
belongs to another project.

## The icon

`gen-icon.py` draws a plain 512x512 PNG: dark background, a grey "splat" of
circles, and "PHOTO SPLAT VR" in a 5x7 bitmap font defined in the script. No font
file, no photo, no third-party asset, and no Apple name or mark — the licensing
rule this project has to satisfy (spec, "Licensing" rule 5). It is a
placeholder: replace it when the name is final, keeping the same rule.

## Build

```sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/title                  # -> build/ps5/title/PPSA99202/ (dev)
make -C platform/ps5/title RELEASE=1        # -> build/ps5/title/PPSA99770/ (release)
make -C platform/ps5/title title-clean
```

`make -C platform/ps5/vk_hello vk_hello-title` does the same from the program's
directory. The payload ELFs (`make -C platform/ps5/vk_hello`) are untouched by
this. The id, concept id and content id come from `identities.json`;
`TITLE_ID=…` selects one of its entries and any other id is refused by
`identity.py`, so there is nothing to keep in sync by hand.

## Four programs, one package

The recipe is shared, not vk_hello's own. A program sets its variables before
including it (`platform/ps5/bench/Makefile` is the example):

| Variable | vk_hello | bench title | viewer title | vr_hello title |
| --- | --- | --- | --- | --- |
| `TITLE_SOURCES` | `main.cpp` | `bench_title.cpp op_audit.cpp da2_bench.cpp sharp_bench.cpp` | `main.cpp videoout.cpp vk_entrypoints.cpp eye_buffers.cpp vr_mode.cpp` | `title.cpp ../hmd2/hmd2.cpp ../viewer/{vk_entrypoints,eye_buffers}.cpp` |
| `TITLE_CPPFLAGS` | `-DPS5_TITLE …` | the same, plus the bench include paths | the same, plus `core/`, RADV's Vulkan headers and `-DSPLAT_HAVE_VULKAN=1` | the same, plus the generated pattern shaders |
| `TITLE_EXTRA_LIBS` / `TITLE_EXTRA_LDFLAGS` | empty | the project + ggml archives (inside the SDK's `--start-group`) and `--wrap=ggml_backend_graph_compute` | `libsplat_core_ps5.a` + `libsplat_miniz.a` | empty |
| `TITLE_EXTRA_LOCAL` | empty | empty | the `vk*` shims, kept out of the dynamic symbol table | the same |

All four write **`build/ps5/title/<TITLE_ID>/`** (the dev build by default):
one title, whose eboot is whichever was built last. What stays per program is
`TITLE_STEM` — it names the object directory, the PIE and `eboot.elf` under
`$(TITLE_BUILD)/`, so the four never share objects or a local-symbol map.
`TITLE_ID` comes from `identities.json`, so no Makefile states an id.

Consequences worth knowing:

- the package holds **whichever program was built last**: the sign step runs on
  every make of a package, so `make -C platform/ps5/vr_hello vr-hello-title`
  after a viewer build leaves the vr_hello eboot in `PPSA99202/`, and the next
  `make -C platform/ps5/viewer` puts the viewer's back. That is the same rule as
  an install (`title.sh` refuses a mismatch only by size; `status` shows `DIFF`);
- `title-clean` (and `clean`) removes the **whole** `PPSA99202/` folder (or the
  release folder with `RELEASE=1`), whoever built it last: after a clean of one
  program, rebuild (or reinstall) the one you want on the console.
- `make -C platform/ps5/vr_hello vr-hello-title` and `bench-title` leave an
  eboot that is not the viewer: an install replaces the tile's program in place.
  `run-bench.sh --title` and `run-vr-hello.sh` print that reminder; `run-viewer.sh`
  reinstalls the viewer on its next run (its `title.sh status` sees the size
  mismatch).
- the viewer's package is the only one with `shaders/`, and nothing removes them
  when another program is built: they stay in the folder (36 KiB) and the bench
  or vr_hello install carries them along, unused. `viewer/verify-package.sh`
  checks the six modules.

Each program decides what it runs; the bench title reads the bench from
`/data/ps5-splat-vr/bench/bench.env` (`docs/research/spike-a.md` §2.4), the
viewer its mode from `viewer.env`. `make -C platform/ps5/viewer` is also the one
that adds the six SPIR-V modules the C6 renderer loads under `shaders/`
(`/app0/shaders` in the title); its runbook step is `docs/console-runbook.md`
§1b.

What the recipe does (`build-title.mk`), and where each input comes from — all
outside the repo, like the RADV archive:

| Step | Input | Default |
| --- | --- | --- |
| compile the program with `-DPS5_TITLE` | the program's source | `../vk_hello/main.cpp` |
| compile the title CRT | `ref/PS5_Vulkan/tooling/native/app_crt.cpp`, `app_cpp_runtime.cpp` | `TITLE_TOOLING` |
| link the PIE | RADV archive + SDK libc++/libunwind + `libps5platform`; `ps5-pie-title.ld` here (the boilerplate's layout + unwind bounds + a page reserved for the process parameters) | `RADV_PS5` |
| convert imports to NIDs | `ps5-native-tool link` (macOS arm64 dev-loop build) | `PS5_NATIVE_TOOL` |
| sign as fSELF | `ps5-native-tool self --sign --magic 0x1D3D154F` | same |
| `sce_module/libc.prx` | a published copy, hash-checked | `TITLE_LIBC_PRX` |
| `param.json` template, `icon0.png` | this directory | `identities.json` + `identity.py` |

The link differs from a payload's in three ways worth knowing:

- the SDK's `crt1.o` is replaced by `_start(process_parameters, loader_teardown)`
  from `app_crt.cpp`; the console's title loader calls it, not `main(argc, argv)`;
- `-L $RADV_SDK/target/lib` is required: the SDK's `libc++.a` names `pthread` as
  a dependent library, which only resolves through that path;
- `lld` is driven directly (as PS5LM and PS5_Vulkan link titles), so the flags
  are lld's own (`--wrap=x`, `--defsym=x=ps5_x`, `--start-group`) rather than
  the `-Wl,…` spellings a payload Makefile passes through the compiler driver.

For a release, build the converter and `libc.prx` from source
(`PS5_Vulkan`'s `make libc`, `tools/build-native.sh`) instead of using the Mac
dev-loop binary and the published module — the research says so, and the hash
check makes the current substitution visible rather than silent.

## Install, launch, stop, inspect

`platform/ps5/scripts/title.sh` does the console side; every verb accepts
`--dry-run` and sends nothing:

```sh
platform/ps5/scripts/title.sh install --dry-run build/ps5/title/PPSA99202   # request list only
platform/ps5/scripts/title.sh install build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh launch  build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh status  build/ps5/title/PPSA99202
platform/ps5/scripts/title.sh stop    build/ps5/title/PPSA99202
```

- `install` copies the folder to `/data/homebrew/<TITLE_ID>/` with ProsperoMgr's
  file API (mkdir + one raw upload per file), verifies every size, then asks
  ShadowMountPlus (`POST /api/v1/scan` on 10101) to register it and waits up to
  90 s for `GET /api/titles` to list it.
- `launch` checks the title is registered and that no other title is running,
  then `POST /api/titles/launch` (ProsperoMgr's
  `sceSystemServiceLaunchApp`), and waits for `GET /api/titles/running`.
- `stop` writes the quit request `/data/ps5-splat-vr/title-quit` and waits; a
  title ends itself with `sceSystemServiceLoadExec("exit", nullptr)`. It never
  kills: the research records losses after killing a rendering title.
- `status` is read-only: registration, the running title, and every file's size
  against the local copy.

Safety is the same as `push-data.sh`: only `/data/homebrew/<TITLE_ID>` and the
identity's data folder (`/data/ps5-splat-vr` for the dev build) are ever
written, `..`/`.`/empty components are rejected before a request is built, and
no delete, rename, chmod or shutdown route is called. `launch`, `stop` and `put`
refuse the release identity `PPSA99770`; `install` and `status` do not touch its
data folder and still work.

## Uninstall

`platform/ps5/scripts/title-uninstall.sh` is the way back out, and
`platform/ps5/title/identities.json` is its allowlist: the script refuses any id
outside the table, and refuses it again unless the `param.json` that is on the
console carries that id and **one of that identity's own names** — for
`PPSA99202` exactly "Photo Splat VR Dev" or the earlier "PS5 Splat VR Viewer",
for `PPSA99770` exactly "Photo Splat VR". So a foreign title that took the id —
Twiso — is refused and its folder is not deleted. It removes the tile whatever
program of ours is installed in it. Add an id to the table before it can be
uninstalled; the proof lives in `identity.py --owns`, tested offline by
`platform/ps5/title/ownership-test.sh`.

```sh
platform/ps5/scripts/title-uninstall.sh PPSA99202            # dry run: GETs only
platform/ps5/scripts/title-uninstall.sh --yes PPSA99202      # stop, unregister, delete, verify
platform/ps5/scripts/title-uninstall.sh --yes --data PPSA99202   # + /data/ps5-splat-vr
```

`--data` deletes only the dev identity's folder. `--data PPSA99770` is refused
before any request: `/data/photo-splat-vr` is the user's private data and no
script in this repo deletes it. A release title that is running is never asked
to quit through a file in that folder; close it on the console instead.

An install leaves four things on the console, and only the first is ours to
delete: `/data/homebrew/<TITLE_ID>/` (our upload), `/user/appmeta/<ID>/` and
`/user/app/<ID>/` (metadata and staging copies ShadowMountPlus writes,
`src/sm_install.c:242-306`), the `/system_ex/app/<ID>` nullfs mount
(`src/sm_filesystem.c:1327-1341`), and the app.db rows that make the tile
(`tbl_contentinfo`/`tbl_appbrowse`). The script removes the folder, asks the
system to drop the rows through ShadowMountPlus's
`POST /api/v1/games/uninstall` -> `sceAppInstUtilAppUnInstall`
(`src/sm_api_service.c:2617-2680`), and verifies the title list and
`/data/homebrew` against a before/after snapshot. It never deletes
`/user/app/<ID>` or `/user/appmeta/<ID>`; it reports them (exit 6) instead.
`docs/console-runbook.md`'s closing section has the operator's view.


## What the console needs (unverified here)

kstuff(-lite), a HEN (OnionHEN/etaHEN), ShadowMountPlus, ProsperoMgr (for the
upload and launch API) and a foreground user signed in. If the first run cannot
write `/data`, add `PPSA99202` to the HEN's app-jailbreak allowlist
(`[app_jailbreak] exact_title_ids`), as PS5LM documents. The full gate, with the
expected log lines, is `docs/console-runbook.md` section 1.
