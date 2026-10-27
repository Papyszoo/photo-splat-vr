#!/usr/bin/env bash
# The W18 SHARP convert spike on the console: one photo converted INSIDE the
# viewer title, next to the live renderer and the panel (S1, in process).
#
#   platform/ps5/scripts/spike.sh                       # golden/immichvr/3.jpg
#   platform/ps5/scripts/spike.sh --photo 4.jpg         # any local .jpg/.png
#   platform/ps5/scripts/spike.sh --scene immichvr-1    # a scene loaded FIRST
#   platform/ps5/scripts/spike.sh --dry-run             # print, send nothing
#
# The run itself is ui-test.sh's: save viewer.env (sha256), write a UI=1 TV
# viewer.env with the spike keys, install the viewer when title.sh does not
# verify it, launch, stream klog, stop, restore viewer.env, pull the PNGs. This
# script adds what a spike needs around it:
#
#   1. upload one photo to <dataDir>/inbox/ui_test_spike.jpg (title.sh
#      put, size-verified; the same name with another size is first removed by
#      its exact path, because it is this script's own file);
#   2. remove a stale <dataDir>/scenes/ui_test_spike folder (exact
#      name only, found by listing every entry so the directory itself is seen)
#      so the conversion writes a fresh one;
#   3. run ui-test.sh with --seconds 90 and the spike keys:
#        CONVERT_SPIKE=<dataDir>/inbox/ui_test_spike.jpg
#        CONVERT_SPIKE_OUT=<dataDir>/scenes/ui_test_spike
#        SCENE=<--scene>   (optional: loaded into the renderer before converting)
#      its end line is the spike's own verdict, `viewer: (PASS|FAIL), exit`,
#      and --pass-regex makes only `viewer: PASS, exit 0` count as the pass;
#   4. read the log's `spike:` lines, print them and keep the PNGs ui-test.sh
#      pulled (convert-start, convert-mid, convert-done, convert-view);
#   5. delete scenes/ui_test_spike and inbox/ui_test_spike.jpg again, by exact
#      name (--keep leaves them), and check viewer.env's sha256 a second time
#      against the copy this script saved before the run.
#
# Nothing else on the console is written or deleted: the two paths above and the
# four shot names are the whole delete list. Exit 0 when the log has
# `spike: S1 PASS <seconds> <splats>`, 1 when it has `spike: S1 FAIL` or a
# `viewer: FAIL` line (a failed run never reads as a pass), 2 usage,
# 4 build/upload/install failed, otherwise ui-test.sh's own code.
#
# Settings: PS5_HOST, PS5_KLOG_PORT (3232), PS5_PROSPERO_PORT (7070),
# SPLAT_DATA_DIR, TITLE_ID (identity, dev by default; the release identity is
# refused). Needs a viewer package at build/ps5/title/<dev id>.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

usage() {  # the header comments above `set -euo pipefail`, uncommented
    awk 'NR > 1 && /^set -euo pipefail/ { exit } NR > 1 { sub(/^# ?/, ""); print }' \
        "${BASH_SOURCE[0]}"
}

dry=0
keep=0
photo=""
scene=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --keep) keep=1 ;;
        --photo)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            photo="$1" ;;
        --photo=*) photo="${1#--photo=}" ;;
        --scene)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            scene="$1" ;;
        --scene=*) scene="${1#--scene=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "spike: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$photo" ]; then
    photo="$(ps5_data_dir)/golden/immichvr/3.jpg"
fi
[ -f "$photo" ] || { echo "spike: no such photo: $photo" >&2; exit 2; }
case "$photo" in
    *.jpg|*.jpeg|*.png) ;;
    *) echo "spike: $photo is not a .jpg/.jpeg/.png" >&2; exit 2 ;;
esac
if [ -n "$scene" ]; then
    case "$scene" in
        ''|*/*|.|..) echo "spike: --scene must be one scene folder name" >&2; exit 2 ;;
    esac
fi

ps5_env_load

note() { printf 'spike: %s\n' "$*" >&2; }
die() { note "$*"; exit "${2:-1}"; }

title_sh="$here/title.sh"
ui_test="$here/ui-test.sh"
title_id="$(ps5_title_id)"
# Agents never launch the release install (AGENTS.md).
ps5_agent_guard_identity "$title_id"
data_root="$(ps5_title_data_dir)"
remote_inbox="$data_root/inbox"
remote_photo="$remote_inbox/ui_test_spike.jpg"
remote_scenes="$data_root/scenes"
remote_scene="$remote_scenes/ui_test_spike"
remote_shots="$data_root/shots"
remote_env="$data_root/bench/viewer.env"
package="$repo/build/ps5/title/$title_id"

file_size() {
    if [ -f "$1" ]; then stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; else echo 0; fi
}
job_state() {  # job_state JOB_ID
    ps5_http GET /api/files/jobs none "" 2>/dev/null | python3 -c '
import json, sys
want = sys.argv[1]
try:
    doc = json.load(sys.stdin)
except Exception:
    print("unknown"); sys.exit(0)
for entry in doc.get("jobs", []):
    if str(entry.get("id")) == want:
        print(entry.get("state", "unknown")); break
else:
    print("unknown")
' "$1"
}
delete_exact() {  # delete_exact REMOTE: one delete job, exactly this path
    local path="$1" job state waited=0
    case "$path" in
        "$remote_photo"|"$remote_scene"|"$remote_shots"/convert-start.png|\
        "$remote_shots"/convert-mid.png|"$remote_shots"/convert-done.png|\
        "$remote_shots"/convert-view.png) ;;
        *) note "refusing to delete '$path' (not one of this spike's files)"; return 1 ;;
    esac
    if [ "$dry" = 1 ]; then
        ps5_dry_line "  POST $(ps5_base_url)/api/ftp/delete body $(ps5_json_path "$path")"
        return 0
    fi
    job="$(ps5_http POST /api/ftp/delete json "$(ps5_json_path "$path")" | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
try:
    print(json.loads(raw).get("job_id", ""))
except Exception:
    pass
')"
    [ -n "$job" ] || { note "delete: no job for $path"; return 1; }
    while [ "$waited" -lt 60 ]; do
        state="$(job_state "$job")"
        case "$state" in
            done) note "deleted $path"; return 0 ;;
            error|attention|canceled)
                note "delete of $path ended $state"; return 1 ;;
        esac
        sleep 1
        waited=$((waited + 1))
    done
    note "delete of $path timed out (job $job)"
    return 1
}

[ -d "$package" ] && [ -f "$package/eboot.bin" ] ||
    die "no title package at $package; make -C platform/ps5/viewer" 4

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ps5splat-spike.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT
# ui-test.sh wants a script; the spike takes no input, so it is one comment (the
# file only gives the run its name in the shots folder).
stub_script="$tmp_dir/w18-spike.txt"
printf '# W18 convert spike: no pad input, the worker drives the panel\n' >"$stub_script"

env_before="$tmp_dir/viewer.env.before"
sha_before=""
if ps5_ftp_get_text "$remote_env" "$env_before" "$dry"; then
    sha_before="$(ps5_sha256 "$env_before")"
    note "saved $remote_env ($(wc -c <"$env_before" | tr -d ' ') bytes, sha256 $sha_before)"
fi

# ---- 1. the photo ------------------------------------------------------------
bytes="$(file_size "$photo")"
previous="$(ps5_ftp_files "$remote_inbox" "$dry" | awk -F'\t' -v want="ui_test_spike.jpg" \
    '$1 == want { print $2; exit }')"
if [ "$dry" = 1 ]; then
    printf '  PUT  %s -> %s (%s bytes)\n' "$photo" "$remote_photo" "$bytes" >&2
elif [ -n "$previous" ] && [ "$previous" = "$bytes" ]; then
    note "$remote_photo already has $bytes bytes; reusing it"
else
    [ -z "$previous" ] || delete_exact "$remote_photo" || die "cannot replace $remote_photo" 4
    note "uploading $photo ($bytes bytes) to $remote_photo"
    "$title_sh" put "$photo" "$remote_photo" || die "upload of the photo failed" 4
fi

# ---- 2. a fresh scene folder and fresh shot names ----------------------------
# write_scene_folder overwrites, so a folder from a failed run is removed by its
# exact name first and the result on the console is always this run's. The
# scene is a DIRECTORY, so this looks at every listed entry, not only files.
if ps5_ftp_has "$remote_scenes" ui_test_spike "$dry"; then
    note "$remote_scene exists; removing it before the conversion"
    delete_exact "$remote_scene" || die "cannot remove the stale $remote_scene" 4
fi
for name in convert-start convert-mid convert-done convert-view; do
    if ps5_ftp_has "$remote_shots" "$name.png" "$dry"; then
        delete_exact "$remote_shots/$name.png" || note "could not remove $remote_shots/$name.png"
    fi
done

# ---- 3. ui-test.sh runs it ---------------------------------------------------
# The run ends on its verdict line (PASS or FAIL) but passes only on the exact
# PASS line; ui-test.sh fails on any FAIL line of its own accord as well.
args=(--seconds 90 --end-regex 'viewer: (PASS|FAIL)[^,]*, exit'
      --pass-regex '^viewer: PASS, exit 0$'
      --env-extra "CONVERT_SPIKE=$remote_photo"
      --env-extra "CONVERT_SPIKE_OUT=$remote_scene")
if [ -n "$scene" ]; then
    args+=(--env-extra "SCENE=$scene")
fi
args+=("$stub_script")
if [ "$dry" = 1 ]; then
    "$ui_test" --dry-run "${args[@]}" || true
    note "dry run: nothing was sent"
    exit 0
fi
note "running ui-test.sh ${args[*]}"
ui_rc=0
"$ui_test" "${args[@]}" 2>&1 | tee "$tmp_dir/ui-test.out" >&2 || ui_rc=$?

# ---- 4. the spike's own lines ------------------------------------------------
out_dir="$(dirname "$(awk '/ui-test: log /{print $3; exit}' "$tmp_dir/ui-test.out" 2>/dev/null)")"
run_log="$out_dir/viewer.log"
if [ ! -f "$run_log" ]; then
    note "no viewer log (ui-test.sh exit $ui_rc); see $tmp_dir/ui-test.out"
    run_log=""
fi
[ -n "$sha_before" ] && cp "$env_before" "$out_dir/viewer.env.before" 2>/dev/null || true
verdict=1
if [ -n "$run_log" ]; then
    note "spike lines from $run_log:"
    awk '/spike: / { i = index($0, "spike: "); print "  " substr($0, i) }' "$run_log" >&2
    # The verdict is the exact line, `spike: S1 PASS <seconds> <splats>`, and a
    # FAIL line always wins: a failed spike never reads as a pass.
    if grep -Eq '^spike: S1 FAIL' "$run_log"; then
        note "FAIL: $(grep -Em1 '^spike: S1 FAIL' "$run_log")"
    elif grep -Eq '^spike: S1 PASS [0-9]+\.[0-9]+ [0-9]+ ' "$run_log"; then
        verdict=0
        note "PASS: $(grep -Em1 '^spike: S1 PASS ' "$run_log")"
    else
        note "FAIL: no \"spike: S1 PASS <seconds> <splats>\" in the log"
    fi
    if grep -q 'DEVICE_LOST\|device lost\|WATCHDOG' "$run_log"; then
        note "the log mentions a device lost or a watchdog line; see $run_log"
    fi
    note "shots and log: $out_dir"
    ls -1 "$out_dir"/*.png 2>/dev/null | sed 's/^/  /' >&2 || true
fi

# ---- 5. back to the normal state --------------------------------------------
if [ "$keep" = 0 ]; then
    delete_exact "$remote_scene" || note "could not remove $remote_scene"
    delete_exact "$remote_photo" || note "could not remove $remote_photo"
else
    note "--keep: $remote_photo and $remote_scene stay on the console"
fi
env_after="$tmp_dir/viewer.env.after"
if [ -n "$sha_before" ]; then
    if ps5_ftp_get_text "$remote_env" "$env_after" "$dry" &&
        [ "$(ps5_sha256 "$env_after")" = "$sha_before" ]; then
        note "viewer.env unchanged (sha256 $sha_before)"
    else
        note "viewer.env differs from the copy saved before the run (ui-test.sh's restore is the authority)"
        verdict=1
    fi
fi
if ! "$title_sh" status "$package" >/dev/null 2>&1; then
    note "the viewer package does not verify on the console; reinstalling"
    "$title_sh" install "$package" || true
fi
note "title.sh status:"
"$title_sh" status "$package" 2>&1 | sed 's/^/  /' >&2 || true

[ "$ui_rc" -eq 0 ] || verdict=1
exit "$verdict"
