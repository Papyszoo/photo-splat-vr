#!/usr/bin/env bash
# A scripted UI run on the TV: save viewer.env, write a UI/SCRIPT viewer.env,
# upload the input script, install/launch the viewer, wait for the script's own
# end, pull the `shot` PNGs read-only, and put viewer.env back (sha256 checked).
#
#   platform/ps5/scripts/ui-test.sh platform/ps5/scripts/ui-tests/browse.txt
#   platform/ps5/scripts/ui-test.sh path/to/script.txt --seconds 12
#   platform/ps5/scripts/ui-test.sh path/to/script.txt --dry-run   # print, send nothing
#
# The script format is core/ui/input_script.h: `<t> press|down|up|hold|stick|
# shot|mark|quit ...`, seconds from the first UI frame. The viewer runs with
# MODE=tv UI=1 SCRIPT=<dataDir>/bench/ui-script.txt SECONDS=<N+30>; its `shot`
# lines write <dataDir>/shots/<name>.png and `quit` logs
# `viewer: script done`. N is --seconds or the script's last event.
#
# <dataDir> is the dev identity's data folder from the table (agents never run
# the release identity: ps5-env.sh refuses it before a request).
#
# In order: ensure <dataDir>/bench, read and save viewer.env (sha256),
# put the new viewer.env and the script, install the viewer only when title.sh
# status does not verify it, stream klog until `viewer: script done` (or a FAIL
# line, or the window), stop, restore viewer.env and verify its sha256, list
# <dataDir>/shots and GET each *.png (no delete/move/write route). The
# PNGs and the klog land in $SPLAT_DATA_DIR/runs/ui_v1/shots/<UTC>-<script>/.
#
# --env-extra KEY=VALUE adds one line to the generated viewer.env, for a run
# that selects its own mode keys instead of a script (W18's CONVERT_SPIKE).
# --end-regex RE is the log line that counts as the run's own end; the default
# is W8's `viewer: script done`. --pass-regex RE is the line that counts as
# PASS (default: --end-regex), so a run that ends on its own verdict
# (W18: `viewer: PASS, exit 0`) passes on that exact line only. A log with any
# `viewer: FAIL` line is a failure whatever those two match.
#
# Exit: 0 the PASS line appeared, 1 it did not (or a FAIL line did), 2 usage/
# missing script, 3 no log stream, 4 put/install/launch failed, 5 the title did
# not stop, 6 viewer.env could not be restored. --dry-run exits 0 after printing.
#
# Settings: PS5_HOST, PS5_KLOG_PORT (3232), SPLAT_DATA_DIR, PS5_PROSPERO_PORT
# (7070). Reused, not reimplemented: title.sh (put/status/install/launch/stop),
# title-capture.py (the shared klog wait) and ps5-env.sh (the HTTP client, the
# encoding and the /api/ftp listing ui-test.sh and spike.sh both read).
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
seconds=""
script=""
extras=()
end_regex='viewer: script done'
pass_regex=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --env-extra)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            extras+=("$1") ;;
        --env-extra=*) extras+=("${1#--env-extra=}") ;;
        --end-regex)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            end_regex="$1" ;;
        --end-regex=*) end_regex="${1#--end-regex=}" ;;
        --pass-regex)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            pass_regex="$1" ;;
        --pass-regex=*) pass_regex="${1#--pass-regex=}" ;;
        --seconds)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            seconds="$1" ;;
        --seconds=*) seconds="${1#--seconds=}" ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "ui-test: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)
            [ -z "$script" ] || { echo "ui-test: one script at a time" >&2; exit 2; }
            script="$1" ;;
    esac
    shift
done

# Every --env-extra must be a KEY=VALUE with a plain shell-identifier key, so it
# cannot smuggle a second entry or a comment into the file.
for extra in "${extras[@]+"${extras[@]}"}"; do
    case "${extra%%=*}" in
        ''|*[!A-Za-z0-9_]*|[0-9]*)
            echo "ui-test: --env-extra '$extra' is not KEY=VALUE with a plain key" >&2
            exit 2 ;;
    esac
    case "$extra" in
        *=*) ;;
        *) echo "ui-test: --env-extra '$extra' has no '='" >&2; exit 2 ;;
    esac
done

[ -n "$script" ] || { usage >&2; exit 2; }
[ -f "$script" ] || { echo "ui-test: no such script: $script" >&2; exit 2; }
[ -s "$script" ] || { echo "ui-test: the script is empty: $script" >&2; exit 2; }
stem="$(basename -- "$script")"
stem="${stem%.txt}"
case "$stem" in
    *[!A-Za-z0-9._-]*|'')
        echo "ui-test: the script name '$stem' is not [A-Za-z0-9._-]+; rename it" >&2
        exit 2 ;;
esac
if [ -n "$seconds" ]; then
    case "$seconds" in
        *[!0-9]*) echo "ui-test: --seconds must be a whole number, got '$seconds'" >&2; exit 2 ;;
    esac
else
    # The last event's end: a `hold` adds its duration, a `press` its 0.1 s tap.
    seconds="$(python3 - "$script" <<'PY'
import sys
last = 0.0
for line in open(sys.argv[1], encoding="utf-8"):
    line = line.split("#", 1)[0].strip()
    parts = line.split()
    if len(parts) < 2:
        continue
    try:
        at = float(parts[0])
    except ValueError:
        continue
    end = at
    if parts[1] == "hold" and len(parts) >= 4:
        try:
            end = at + float(parts[3])
        except ValueError:
            pass
    elif parts[1] == "press":
        end = at + 0.1
    last = max(last, end)
print(int(last + 0.999))
PY
)"
fi
[ "$seconds" -gt 0 ] || { echo "ui-test: --seconds must be at least 1" >&2; exit 2; }

ps5_env_load

note() { printf 'ui-test: %s\n' "$*" >&2; }
die() { note "$*"; exit "${2:-1}"; }

title_id="$(ps5_title_id)"
# Agents never launch the release install (AGENTS.md).
ps5_agent_guard_identity "$title_id"
data_root="$(ps5_title_data_dir)"
title_sh="$here/title.sh"
klog_sh="$here/klog.sh"
capture="$here/title-capture.py"
package="$repo/build/ps5/title/$title_id"

remote_env="$data_root/bench/viewer.env"
remote_bench="$data_root/bench"
remote_script="$remote_bench/ui-script.txt"
remote_shots="$data_root/shots"

# The viewer's own SECONDS: the script's length plus 30 s of slack, so a script
# that never reaches `quit` still ends the app before the capture window.
run_seconds=$((seconds + 30))
window=$((run_seconds + 120))

utc="$(date -u +%Y%m%dT%H%M%SZ)"
out_dir="$(ps5_data_dir)/runs/ui_v1/shots/$utc-$stem"
log="$out_dir/viewer.log"

# ---- remote helpers (GET/POST only where title.sh has no verb) ---------------
# The encoding, the sha256 and /api/ftp/text live in ps5-env.sh, so spike.sh
# reads them the same way. Printed requests go to fd 9, this script's dry-run
# stream (stdout carries the pulled PNG paths).
exec 9>&1
PS5_DRY_FD=9

fetch_binary() {  # fetch_binary REMOTE LOCAL -> 0 when the bytes were written
    local remote="$1" local_file="$2"
    if [ "$dry" = 1 ]; then
        printf '  GET  %s/api/ftp/download?path=%s -> %s\n' \
               "$(ps5_base_url)" "$(ps5_url_encode "$remote")" "$local_file" >&9
        return 0
    fi
    # Straight to the file: a PNG has NUL bytes and a shell variable cannot
    # carry those, so this must not go through "$(...)".
    ps5_http GET "/api/ftp/download?path=$(ps5_url_encode "$remote")" none "" \
        >"$local_file" 2>/dev/null || return 1
    [ -s "$local_file" ]
}

ensure_remote_dir() {  # ensure_remote_dir DIR
    if [ "$dry" = 1 ]; then
        printf '  GET  %s/api/ftp/list?path=%s   (mkdir below only when this fails)\n' \
               "$(ps5_base_url)" "$(ps5_url_encode "$1")" >&9
        printf '  POST %s/api/ftp/mkdir  body %s\n' "$(ps5_base_url)" "$(ps5_json_path "$1")" >&9
        return 0
    fi
    ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$1")" none "" >/dev/null 2>&1 && return 0
    note "mkdir $1"
    ps5_http POST /api/ftp/mkdir json "$(ps5_json_path "$1")" >/dev/null ||
        die "cannot create $1" 4
}

remote_pngs() {  # remote_pngs DIR -> names, one per line, empty when absent
    ps5_ftp_files "$1" "$dry" | awk -F'\t' '$1 ~ /\.png$/ { print $1 }'
}

install_if_needed() {  # install_if_needed [NOTE]
    if [ "$dry" = 1 ]; then
        if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
            "$title_sh" status --dry-run "$package" | sed 's/^/  /'
            "$title_sh" install --dry-run "$package"
        else
            note "no package at $package yet; no install request to print"
        fi
        return 0
    fi
    note "install $title_id (viewer)"
    if "$title_sh" status "$package" >/dev/null 2>&1; then
        note "package already installed and verified; skipping the upload"
        return 0
    fi
    install_files
}

install_files() {  # 0 when the console copy verifies (ShadowMountPlus caveat)
    local rc=0
    "$title_sh" install "$package" || rc=$?
    if [ "$rc" -eq 0 ]; then
        return 0
    fi
    if "$title_sh" status "$package" >/dev/null 2>&1; then
        note "install: title.sh exited $rc at the ShadowMountPlus scan, but the files verify and $title_id is registered; continuing"
        return 0
    fi
    note "install failed (title.sh exit $rc)"
    return "$rc"
}

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ps5splat-uitest.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT
viewer_before="$tmp_dir/viewer.env"
viewer_sha_before=""
viewer_present_before=0
viewer_env="$tmp_dir/viewer.env.new"
{
    printf '# written by ui-test.sh at %s (script %s)\nMODE=tv\nUI=1\nSCRIPT=%s\nSECONDS=%s\n' \
        "$utc" "$stem" "$remote_script" "$run_seconds"
    for extra in "${extras[@]+"${extras[@]}"}"; do
        printf '%s\n' "$extra"
    done
} >"$viewer_env"

if [ "$dry" = 1 ]; then
    note "title route: $title_id ($package), script $script"
    note "script $stem: ${seconds}s of script, SECONDS=$run_seconds, window ${window}s"
    note "shots and log: $out_dir"
    note "1. save $remote_env (sha256 compared after the restore)"
    ps5_ftp_get_text "$remote_env" "$viewer_before" "$dry" || true
    note "2. ensure the bench directory and write the UI selector ($(wc -c <"$viewer_env" | tr -d ' ') bytes)"
    ensure_remote_dir "$remote_bench"
    sed 's/^/    /' "$viewer_env"
    "$title_sh" put --dry-run "$viewer_env" "$remote_env"
    note "3. upload the script"
    "$title_sh" put --dry-run "$script" "$remote_script"
    note "4. install (skipped when title.sh status already verifies the package)"
    install_if_needed
    note "5. launch and stream klog -> $log until /viewer: (script done|FAIL|done, exit)/ or ${window}s"
    if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
        "$title_sh" launch --dry-run "$package" | sed 's/^/  /'
    else
        note "  launch (no package yet)"
    fi
    note "6. stop"
    if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
        "$title_sh" stop --dry-run "$package" | sed 's/^/  /'
    else
        note "  stop (no package yet)"
    fi
    note "7. pull shots read-only: GET /api/ftp/list?path=$remote_shots, then"
    printf '  GET  %s/api/ftp/download?path=%s/<name>.png -> %s/<name>.png\n' \
           "$(ps5_base_url)" "$(ps5_url_encode "$remote_shots")" "$out_dir" >&9
    note "   (the live run prints every pulled path)"
    note "8. restore $remote_env from the copy saved in step 1, only when its sha256 changed"
    printf '  POST %s/api/ftp/upload?path=%s&overwrite=1  body <the saved viewer.env>\n' \
           "$(ps5_base_url)" "$(ps5_url_encode "$remote_env")" >&9
    printf '  GET  %s/api/ftp/text?path=%s   (verify the restored sha256)\n' \
           "$(ps5_base_url)" "$(ps5_url_encode "$remote_env")" >&9
    note "dry run: nothing was sent"
    exit 0
fi

: "${PS5_HOST:?ui-test: set PS5_HOST or add PS5_HOST=... to .env.local}"
[ -d "$package" ] && [ -f "$package/eboot.bin" ] ||
    die "no title package at $package; make -C platform/ps5/viewer" 2

mkdir -p "$out_dir"

# ---- 1. viewer.env as it was ------------------------------------------------
if ps5_ftp_get_text "$remote_env" "$viewer_before" "$dry"; then
    viewer_present_before=1
    viewer_sha_before="$(ps5_sha256 "$viewer_before")"
    note "saved $remote_env ($(wc -c <"$viewer_before" | tr -d ' ') bytes, sha256 $viewer_sha_before)"
else
    note "no readable $remote_env on the console; there will be nothing to restore"
fi

# ---- 2/3. the selector and the script ---------------------------------------
ensure_remote_dir "$remote_bench"
note "writing $remote_env (MODE=tv UI=1 SCRIPT=$remote_script SECONDS=$run_seconds${extras[0]:+ +env-extra})"
"$title_sh" put "$viewer_env" "$remote_env" || die "writing viewer.env failed" 4
note "uploading $script -> $remote_script"
"$title_sh" put "$script" "$remote_script" || die "uploading the script failed" 4

# ---- 4/5. install, launch and wait ------------------------------------------
install_if_needed || die "the viewer is not installed" 4
note "log $log"
capture_rc=0
PS5_RUN_TAG=ui-test PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" \
PS5_RUN_SECONDS="$window" PS5_RUN_OUT="$log" \
PS5_RUN_TITLE_SH="$title_sh" PS5_RUN_PACKAGE="$package" PS5_RUN_KLOG="$klog_sh" \
PS5_RUN_END='viewer: (?:script done|FAIL|done, exit)' \
python3 "$capture" || capture_rc=$?

# The title ends itself on `quit`; stop is the quit file for a script that never
# reached it, and never a kill.
stop_rc=0
"$title_sh" stop "$package" || stop_rc=$?

# ---- 6. viewer.env back -----------------------------------------------------
viewer_after="$tmp_dir/viewer.env.after"
viewer_sha_after=""
if ps5_ftp_get_text "$remote_env" "$viewer_after" "$dry"; then
    viewer_sha_after="$(ps5_sha256 "$viewer_after")"
fi
if [ "$viewer_present_before" = 1 ]; then
    if [ "$viewer_sha_after" = "$viewer_sha_before" ]; then
        note "viewer.env unchanged (sha256 $viewer_sha_after)"
    else
        note "viewer.env changed (sha256 ${viewer_sha_after:-absent}); putting the saved copy back"
        "$title_sh" put "$viewer_before" "$remote_env" || die "restoring viewer.env failed" 6
        ps5_ftp_get_text "$remote_env" "$viewer_after" "$dry" &&
            [ "$(ps5_sha256 "$viewer_after")" = "$viewer_sha_before" ] ||
            die "viewer.env still differs from the saved copy after the restore" 6
        note "viewer.env restored (sha256 $viewer_sha_before)"
    fi
elif [ -n "$viewer_sha_after" ]; then
    note "warning: viewer.env did not exist before this run and exists now; no copy to restore"
fi

# ---- 7. the shots (read-only) -----------------------------------------------
pulled=0
while IFS= read -r name; do
    [ -n "$name" ] || continue
    case "$name" in
        */*) note "skipping $name (not a file name)"; continue ;;
    esac
    if fetch_binary "$remote_shots/$name" "$out_dir/$name"; then
        note "pulled $out_dir/$name ($(wc -c <"$out_dir/$name" | tr -d ' ') bytes)"
        printf '%s\n' "$out_dir/$name"
        pulled=$((pulled + 1))
    else
        note "could not pull $remote_shots/$name"
    fi
done <<EOF
$(remote_pngs "$remote_shots")
EOF
if [ "$pulled" -eq 0 ]; then
    note "no PNG was pulled from $remote_shots"
fi

# ---- verdict ----------------------------------------------------------------
# The PASS line is the end line unless --pass-regex names another one: a run
# that ends on its own verdict (spike.sh ends on PASS or FAIL) must not read as
# a pass, and a FAIL line is a failure whatever those two match.
pass_re="${pass_regex:-$end_regex}"
if [ "$capture_rc" -ne 0 ]; then
    note "the run did not reach its end line (capture exit $capture_rc); see $log"
    exit "$capture_rc"
fi
if [ "$stop_rc" -ne 0 ]; then
    note "the run finished but the title did not stop (title.sh exit $stop_rc)"
    exit 5
fi
if grep -Eq '^viewer: .*FAIL' "$log" 2>/dev/null; then
    note "FAIL: the log has a \"$(grep -Em1 '^viewer: .*FAIL' "$log")\" line; see $log"
    exit 1
fi
if grep -Eq "$pass_re" "$log" 2>/dev/null; then
    note "PASS: $pulled shot(s) in $out_dir"
    exit 0
fi
note "FAIL: no \"$pass_re\" in $log"
exit 1
