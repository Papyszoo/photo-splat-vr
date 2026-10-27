#!/usr/bin/env bash
# The viewer title on the console: install (if needed), launch and watch it.
# MODE=vr (the default) puts the scene in the PSVR2; --mode tv is the old
# 1920x1080 TV run.
#
#   platform/ps5/scripts/run-viewer.sh                     # VR, 30 min cap, first scene
#   platform/ps5/scripts/run-viewer.sh --seconds 600       # VR, quit by 10 min at the latest
#   platform/ps5/scripts/run-viewer.sh --scene 02_living_lite
#   platform/ps5/scripts/run-viewer.sh --mode tv --seconds 60
#   platform/ps5/scripts/run-viewer.sh --dry-run           # print every step, send nothing
#   platform/ps5/scripts/run-viewer.sh --env driver.env    # extra settings
#
# The viewer title (platform/ps5/viewer, id from platform/ps5/title/identities.json,
# the dev identity) reads its input from <dataDir>/bench/viewer.env because a
# title launched through ProsperoMgr gets no argv. <dataDir> is that identity's
# data folder; agents never run the release identity (ps5-env.sh refuses it).
#
#   MODE=vr              vr (PSVR2, default) or tv (mono 1920x1080 VideoOut)
#   SECONDS=1800         VR: hard cap, at most 1800 s; Options quits earlier.
#                        TV: run length; 0 = until title.sh stop (default 60)
#   SCENE=02_living_lite scene folder under <dataDir>/scenes (default: first);
#                        in VR, L1/R1 step through all of them, sorted by name
#   COMFORT_RADIUS=0.25  metres; fade-out distance from the capture point
#   WORLD_SCALE=1.0      VR only: scene size around the capture point
#   PRELOAD=1            VR only: 0 disables preloading the next scene
#   WIDTH=1920           TV only: VideoOut buffer size (2560x1440, 3840x2160)
#   HEIGHT=1080
#   FLIP_RATE=1          TV only: 0 every vblank, 1 every second (30 fps), 2 every third
#   SWAY_PERIOD=12       TV only: seconds for one side-to-side cycle
#
# The script prints the scene folders it finds on the console before launching,
# so it is clear what L1/R1 will step through.
#
# --env FILE appends the entries of a local file (RADV_DEBUG, MESA_* driver
# settings, any of the keys above) after the generated ones; a SECONDS=,
# SCENE= or MODE= line in the template is dropped so the flags win.
#
# Scenes are pushed first with: platform/ps5/scripts/push-data.sh --scenes DIR
# (or --scene DIR), which also creates <dataDir>/bench for viewer.env.
#
# In order: write viewer.env (title.sh put), install the package when
# `title.sh status` does not verify it, list the scenes, connect the klog
# stream, launch, capture until "viewer: done, exit" or "viewer: FAIL" (or the
# window closes), then `title.sh stop` (a no-op after a normal end - it never
# kills). The capture keeps streaming until the window closes even after the
# end line (title-capture.py), so a short --seconds also keeps the wait short.
#
# Exit: 0 the end line appeared, 1 it did not within the window, 2 usage or a
# missing package/file, 3 no log stream, 4 put/install/launch failed, 5 the
# capture stayed empty or the title did not stop.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"
ps5_env_load

usage() {
    sed -n '2,52p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

dry=0
mode=""
scene=""
seconds=""
window=""
package=""
env_file=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --mode)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            mode="$1" ;;
        --mode=*) mode="${1#--mode=}" ;;
        --scene)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            scene="$1" ;;
        --scene=*) scene="${1#--scene=}" ;;
        --seconds)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            seconds="$1" ;;
        --seconds=*) seconds="${1#--seconds=}" ;;
        --window)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            window="$1" ;;
        --package)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            package="$1" ;;
        --env)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            env_file="$1" ;;
        --env=*) env_file="${1#--env=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "run-viewer: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# MODE=vr is the default; tv keeps the old mono VideoOut run.
[ -n "$mode" ] || mode=vr
if [ "$mode" != "vr" ] && [ "$mode" != "tv" ]; then
    echo "run-viewer: --mode must be vr or tv, got '$mode'" >&2
    exit 2
fi
# The viewer's own keys; a scene folder name has to be usable as a folder name.
if [ -n "$scene" ]; then
    case "$scene" in
        *[!A-Za-z0-9._-]*) echo "run-viewer: --scene must be a scene folder name ([A-Za-z0-9._-]+), got '$scene'" >&2; exit 2 ;;
    esac
fi
if [ -n "$seconds" ]; then
    case "$seconds" in
        *[!0-9]*) echo "run-viewer: --seconds must be a whole number, got '$seconds'" >&2; exit 2 ;;
    esac
elif [ "$mode" = "vr" ]; then
    # The viewer's own 30-minute cap; SECONDS is only written when --seconds is
    # given, so the app's default stays the cap.
    seconds=1800
else
    seconds=60
fi
if [ "$mode" = "vr" ] && [ "$seconds" -gt 1800 ]; then
    echo "run-viewer: --seconds $seconds is over the VR cap; using 1800" >&2
    seconds=1800
fi
if [ -n "$window" ]; then
    case "$window" in
        *[!0-9]*) echo "run-viewer: --window must be a whole number, got '$window'" >&2; exit 2 ;;
    esac
else
    # The run itself plus room for scene load, device and shader-cache warm-up
    # and teardown; an unbounded TV run (SECONDS=0) gets 10 minutes.
    if [ "$seconds" -eq 0 ]; then window=600; else window=$((seconds + 90)); fi
fi
if [ -n "$env_file" ] && [ ! -f "$env_file" ]; then
    echo "run-viewer: no such --env file: $env_file" >&2
    exit 2
fi

note() { printf 'run-viewer: %s\n' "$*" >&2; }

title_id="$(ps5_title_id)"
# Agents never launch the release install (AGENTS.md): refuse the selected
# identity, and a --package built as the release one, before any request.
ps5_agent_guard_identity "$title_id"
data_root="$(ps5_title_data_dir)"
[ -n "$package" ] || package="$repo/build/ps5/title/$title_id"
package_id="$(ps5_package_title_id "$package")" || package_id=""
[ -z "$package_id" ] || ps5_agent_guard_identity "$package_id"
[ -d "$package" ] && [ -f "$package/eboot.bin" ] ||
    { echo "run-viewer: no title package at $package; make -C platform/ps5/viewer" >&2; exit 2; }
[ -f "$package/shaders/project.comp.spv" ] ||
    { echo "run-viewer: $package has no shaders/; make -C platform/ps5/viewer" >&2; exit 2; }

title_sh="$here/title.sh"
klog_sh="$here/klog.sh"
remote_env="$data_root/bench/viewer.env"

tmp_env="$(mktemp "${TMPDIR:-/tmp}/ps5splat-viewer-env.XXXXXX")"
trap 'rm -f "$tmp_env"' EXIT
{
    printf '# written by run-viewer.sh at %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'MODE=%s\n' "$mode"
    # VR without --seconds writes the 1800 s cap explicitly (the title also
    # defaults to it when SECONDS is absent, for a direct launch).
    printf 'SECONDS=%s\n' "$seconds"
    [ -z "$scene" ] || printf 'SCENE=%s\n' "$scene"
    if [ -n "$env_file" ]; then
        # The flags above win over the template for the run's own keys.
        grep -v -E '^[[:space:]]*(MODE|SECONDS|SCENE)[[:space:]]*=' "$env_file" || true
    fi
} >"$tmp_env"

# The scene folders on the console, one per line: what L1/R1 step through, in
# the order ScenePlaylist sees them (sorted by name). Empty when the console is
# not answering; the viewer then says "scenes: 0 folder(s)" itself.
scene_list() {
    local body
    body=$(ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$data_root/scenes")" none "" 2>/dev/null) || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("entries", []):
    if entry.get("is_dir"):
        print(entry.get("name", ""))
'
}

scene_rows() {  # scene_rows SCENE_FROM_FLAG -> numbered list, empty on no scenes
    scene_list | awk -v want="$1" '
        { printf "  %2d. %s", NR, $0
          if ($0 == want) printf "   <- --scene"
          else if (NR == 1 && want == "") printf "   <- default (first by name)"
          printf "\n" }'
}

out_dir="$(ps5_data_dir)/runs/console"
log="$out_dir/$(date -u +%Y%m%dT%H%M%SZ)-viewer-title.log"

if [ "$dry" = 1 ]; then
    note "title route: $title_id ($package), mode $mode, ${seconds}s cap/run, ${window}s capture window"
    note "1. input: $remote_env"
    printf '  body %s bytes:\n' "$(wc -c <"$tmp_env" | tr -d ' ')"
    sed 's/^/    /' "$tmp_env"
    "$title_sh" put --dry-run "$tmp_env" "$remote_env"
    note "2. install (skipped when title.sh status already verifies the package)"
    "$title_sh" status --dry-run "$package" | sed 's/^/  /'
    "$title_sh" install --dry-run "$package"
    note "3. scenes on the console (L1/R1 step through them${mode:+ in VR mode}):"
    printf '  GET  http://%s/api/ftp/list?path=%s\n' \
        "${PS5_HOST:-<PS5_HOST>}:${PS5_PROSPERO_PORT:-7070}" \
        "$(ps5_url_encode "$data_root/scenes")"
    note "   (the live run prints the folder names here; scene folder names go in --scene)"
    note "4. launch"
    "$title_sh" launch --dry-run "$package" | sed 's/^/  /'
    note "5. stream klog -> $log until /^viewer: (done, exit|FAIL|quit requested)/ or ${window}s"
    note "6. stop"
    "$title_sh" stop --dry-run "$package" | sed 's/^/  /'
    note "dry run: nothing was sent"
    exit 0
fi

: "${PS5_HOST:?run-viewer: set PS5_HOST or add PS5_HOST=... to .env.local}"

note "writing $remote_env (MODE=$mode, SECONDS=$seconds${scene:+, SCENE=$scene})"
"$title_sh" put "$tmp_env" "$remote_env" || exit 4

if "$title_sh" status "$package" >/dev/null 2>&1; then
    note "package already installed and verified (title.sh status); skipping install"
else
    note "installing $package ($(du -sk "$package" | awk '{printf "%.1f MiB", $1/1024}'))"
    "$title_sh" install "$package" || exit 4
fi

rows="$(scene_rows "$scene")"
if [ -n "$rows" ]; then
    note "scenes in $data_root/scenes (L1/R1 step through them):"
    printf '%s\n' "$rows" >&2
else
    note "no scene folders listed on the console; the viewer needs at least one (push-data.sh --scenes DIR)"
fi

mkdir -p "$out_dir"
note "log $log"

# klog first, then launch (klogsrv forwards live only, it replays nothing), then
# read until the viewer's end line. title-capture.py is shared with
# run-vr-hello.sh: one launch/capture/error contract for every title run.
capture_rc=0
PS5_RUN_TAG=run-viewer PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" \
PS5_RUN_SECONDS="$window" PS5_RUN_OUT="$log" \
PS5_RUN_TITLE_SH="$title_sh" PS5_RUN_PACKAGE="$package" PS5_RUN_KLOG="$klog_sh" \
PS5_RUN_END='viewer: (?:done, exit|FAIL|quit requested)' \
python3 "$here/title-capture.py" || capture_rc=$?

# A viewer that ended normally has already ended itself; stop is then a no-op.
# A viewer still swaying (SECONDS=0) ends here through the quit file.
stop_rc=0
"$title_sh" stop "$package" || stop_rc=$?

if [ "$capture_rc" -ne 0 ]; then
    note "the viewer did not finish (exit $capture_rc); capture in $log"
    exit "$capture_rc"
fi
if [ "$stop_rc" -ne 0 ]; then
    note "the viewer finished but the title did not stop (title.sh exit $stop_rc)"
    exit 5
fi
note "done: the viewer reached its end line; capture in $log"
exit 0
