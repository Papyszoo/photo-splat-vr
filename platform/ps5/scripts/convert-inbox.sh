#!/usr/bin/env bash
# Convert photos on the console: upload them to the console inbox, run the
# bench title's convert mode (BENCH=convert, platform/ps5/bench/convert_inbox.cpp)
# until every photo has a scene folder, then put the viewer build back.
#
#   platform/ps5/scripts/convert-inbox.sh                          # convert what is in the inbox
#   platform/ps5/scripts/convert-inbox.sh ~/Photos/immichvr-4.jpg  # upload, then convert
#   platform/ps5/scripts/convert-inbox.sh --dry-run photo1.jpg     # every step, nothing sent
#   platform/ps5/scripts/convert-inbox.sh --window 2400 photo1.jpg # a longer klog window
#
# What it does, in order:
#   1. makes the convert build: make -C platform/ps5/bench bench-title
#      (the dev identity, the same title as the viewer);
#   2. creates <dataDir>/inbox when it is missing and uploads each
#      given photo there with title.sh put (same file API, size verified). A
#      photo already in the inbox with the same size is left alone; one with a
#      different size is refused, because the inbox is never overwritten,
#      moved or deleted;
#   3. reads <dataDir>/bench/viewer.env first and keeps a copy and its sha256,
#      then writes BENCH=convert to bench.env;
#   4. installs the convert build into the dev identity and runs it with klog captured
#      to $SPLAT_DATA_DIR/runs/console/<UTC>-convert-title.log until the title's
#      own "convert: done, exit" line (or the window closes), then stops it;
#   5. rebuilds and reinstalls the viewer build (make -C platform/ps5/viewer),
#      so the console is left with the viewer in the dev identity;
#   6. reads viewer.env again, compares its sha256 with the saved one and puts
#      the saved copy back when it changed.
#
# The console side never deletes or moves an inbox photo, and never overwrites a
# scene folder: a photo whose <dataDir>/scenes/<stem>/ already exists is
# skipped, so re-running converts only what is new. A photo that fails is logged
# and skipped by the title, and the batch continues.
#
# On this console ShadowMountPlus binds its API to 127.0.0.1, so `title.sh
# install` can end exit 3 AT its scan after uploading and verifying every file.
# That is not a failed install here: the one app is already registered, and the
# script checks `title.sh status` (files on the console + registration) before it
# treats an install as failed.
#
# Settings: PS5_HOST, PS5_KLOG_PORT (3232), SPLAT_DATA_DIR, TITLE_ID (identity
# selection, dev by default; the release identity is refused). The build needs
# PS5_PAYLOAD_SDK (default $SPLAT_DATA_DIR/ref/radv-ps5/sdk) and LLVM_CONFIG
# (the SDK's clang wrapper; the usual Homebrew path is used when unset).
#
# Reused, not reimplemented: title.sh (put/status/install/launch/stop), the
# shared klog capture in title-capture.py and the ProsperoMgr client in
# ps5-env.sh (the same routes push-data.sh sends for its directories).
#
# Exit: 0 the run ended with "convert: PASS" and the viewer is back; 1 it did
# not (no end line in the window, or the title's own FAIL), 2 usage/a missing or
# unsafe local photo/a refusal to overwrite an inbox photo, 3 no log stream,
# 4 build/put/install/launch failed, 5 the capture stayed empty or the title did
# not stop, 6 the viewer build or viewer.env could not be restored.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

usage() {
    sed -n '2,58p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

dry=0
window=""
photos=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --window)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            window="$1" ;;
        --window=*) window="${1#--window=}" ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "convert-inbox: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *) photos+=("$1") ;;
    esac
    shift
done

if [ -n "$window" ]; then
    case "$window" in
        *[!0-9]*) echo "convert-inbox: --window must be a whole number, got '$window'" >&2; exit 2 ;;
    esac
else
    # Model load (~60 s) plus one photo per minute, with room to spare; the
    # capture stops at the title's own end line, so this is only the ceiling.
    window=1800
fi

ps5_env_load

note() { printf 'convert-inbox: %s\n' "$*" >&2; }
die() { note "$*"; exit "${2:-1}"; }

title_id="$(ps5_title_id)"
# Agents never run the release install (AGENTS.md): refuse it before a build, a
# local file read or a request.
ps5_agent_guard_identity "$title_id"
data_root="$(ps5_title_data_dir)"
title_sh="$here/title.sh"
klog_sh="$here/klog.sh"
capture="$here/title-capture.py"
package="$repo/build/ps5/title/$title_id"
bench_dir="$repo/platform/ps5/bench"
viewer_dir="$repo/platform/ps5/viewer"
remote_inbox="$data_root/inbox"
remote_bench_env="$data_root/bench/bench.env"
remote_viewer_env="$data_root/bench/viewer.env"
prospero_port="${PS5_PROSPERO_PORT:-7070}"

# The cross build inputs are external (AGENTS.md): the SDK and the LLVM the
# title toolchain wraps. Defaults live under SPLAT_DATA_DIR, never in the repo.
: "${PS5_PAYLOAD_SDK:=$(ps5_data_dir)/ref/radv-ps5/sdk}"
if [ -z "${LLVM_CONFIG:-}" ]; then
    for candidate in /opt/homebrew/opt/llvm@21/bin/llvm-config /opt/homebrew/opt/llvm/bin/llvm-config; do
        [ -x "$candidate" ] && LLVM_CONFIG="$candidate" && break
    done
fi
export PS5_PAYLOAD_SDK
[ -z "${LLVM_CONFIG:-}" ] || export LLVM_CONFIG

base_url() {  # http://host:port
    printf 'http://%s:%s' "${PS5_HOST:-<PS5_HOST>}" "$prospero_port"
}
file_size() {
    if [ -f "$1" ]; then stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; else echo 0; fi
}
human() {
    python3 -c 'import sys; n=float(sys.argv[1]); u=["B","KiB","MiB","GiB"]; i=0
while n>=1024 and i<3: n/=1024; i+=1
print("%.2f %s" % (n, u[i]))' "$1"
}
sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        echo "no-sha256-tool"
    fi
}

# ---- local photos ------------------------------------------------------------
# The remote name is the local basename, so the scene folder is that stem. Only
# names that are safe in a remote path (and that the title will actually pick
# up: a lowercase .jpg/.jpeg/.png) are accepted.
for photo in "${photos[@]+"${photos[@]}"}"; do
    [ -f "$photo" ] || { note "no such file: $photo"; exit 2; }
    name="$(basename -- "$photo")"
    case "$name" in
        *[!A-Za-z0-9._-]*)
            note "$name is not [A-Za-z0-9._-]+, so it cannot be an inbox name; rename it first"
            exit 2 ;;
    esac
    case "$name" in
        *.jpg|*.jpeg|*.png) ;;
        *)
            note "$name: convert mode reads .jpg, .jpeg and .png only (lowercase)"
            exit 2 ;;
    esac
done
# Two inputs with one basename would upload the second over the first (the
# inbox photo is never replaced, so this is a usage error).
for ((i = 0; i < ${#photos[@]}; i++)); do
    for ((j = i + 1; j < ${#photos[@]}; j++)); do
        [ "$(basename -- "${photos[$i]}")" != "$(basename -- "${photos[$j]}")" ] ||
            { note "two photos are both named $(basename -- "${photos[$i]}"); rename one first"; exit 2; }
    done
done

# ---- remote helpers ----------------------------------------------------------
# A GET/POST pair only where title.sh has no verb for it: creating the inbox
# directory and reading viewer.env back. They use ps5-env.sh's client, i.e. the
# same ProsperoMgr routes push-data.sh sends.
exec 9>&1
fetch_text() {  # fetch_text REMOTE LOCAL -> 0 when the console returned a file
    local remote="$1" local_file="$2" body
    if [ "$dry" = 1 ]; then
        printf '  GET  %s/api/ftp/text?path=%s\n' "$(base_url)" "$(ps5_url_encode "$remote")" >&9
        return 1
    fi
    body="$(ps5_http GET "/api/ftp/text?path=$(ps5_url_encode "$remote")" none "" 2>/dev/null)" || return 1
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    sys.stdout.write(json.load(sys.stdin).get("text", ""))
except Exception:
    sys.exit(1)
' >"$local_file" || return 1
}

ensure_remote_dir() {  # ensure_remote_dir DIR
    if [ "$dry" = 1 ]; then
        printf '  GET  %s/api/ftp/list?path=%s   (mkdir below only when this fails)\n' \
               "$(base_url)" "$(ps5_url_encode "$1")" >&9
        printf '  POST %s/api/ftp/mkdir  body %s\n' "$(base_url)" "$(ps5_json_path "$1")" >&9
        return 0
    fi
    ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$1")" none "" >/dev/null 2>&1 && return 0
    note "mkdir $1"
    ps5_http POST /api/ftp/mkdir json "$(ps5_json_path "$1")" >/dev/null ||
        die "cannot create $1" 4
}

remote_files() {  # remote_files DIR -> "name<TAB>size", empty when absent
    local body
    body="$(ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$1")" none "" 2>/dev/null)" || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("entries", []):
    if not entry.get("is_dir"):
        print("%s\t%d" % (entry.get("name", ""), int(entry.get("size", 0))))
'
}

install_if_needed() {  # install_if_needed LABEL
    if [ "$dry" = 1 ]; then
        if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
            "$title_sh" status --dry-run "$package" | sed 's/^/  /'
            "$title_sh" install --dry-run "$package"
        else
            note "no package at $package yet (the make step fills it); no install request to print"
        fi
        return 0
    fi
    note "install $title_id ($1 build)"
    if "$title_sh" status "$package" >/dev/null 2>&1; then
        note "package already installed and verified; skipping the upload"
        return 0
    fi
    install_files "$1"
}

install_files() {  # install_files LABEL -> 0 when the console copy verifies
    local label="$1" rc=0
    "$title_sh" install "$package" || rc=$?
    if [ "$rc" -eq 0 ]; then
        return 0
    fi
    # On this console ShadowMountPlus binds 10101 to 127.0.0.1, so install can
    # end exit 3 AT its scan, after every file was uploaded and size-verified
    # (docs/console-runbook.md, cleanup). The one app is already registered in
    # that case, and status proves both facts; anything else is a real failure.
    if "$title_sh" status "$package" >/dev/null 2>&1; then
        note "install $label: title.sh exited $rc at the ShadowMountPlus scan, but the files verify and $title_id is registered; continuing"
        return 0
    fi
    note "install $label failed (title.sh exit $rc)"
    return "$rc"
}

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/ps5splat-convert.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT
viewer_before="$tmp_dir/viewer.env"
viewer_sha_before=""
viewer_present_before=0
bench_env="$tmp_dir/bench.env"
printf '# written by convert-inbox.sh at %s\nBENCH=convert\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    >"$bench_env"

out_dir="$(ps5_data_dir)/runs/console"
log="$out_dir/$(date -u +%Y%m%dT%H%M%SZ)-convert-title.log"

if [ "$dry" = 1 ]; then
    note "convert route: $title_id ($package), ${#photos[@]} photo(s), window ${window}s"
    note "1. build the convert mode into the one title"
    note "  make -C $bench_dir bench-title TITLE_ID=$title_id    (PS5_PAYLOAD_SDK=$PS5_PAYLOAD_SDK, LLVM_CONFIG=${LLVM_CONFIG:-<unset>})"
    note "2. inbox $remote_inbox"
    ensure_remote_dir "$remote_inbox"
    ensure_remote_dir "$data_root/bench"
    for photo in "${photos[@]+"${photos[@]}"}"; do
        name="$(basename -- "$photo")"
        "$title_sh" put --dry-run "$photo" "$remote_inbox/$name"
    done
    note "3. keep viewer.env and write the selector"
    fetch_text "$remote_viewer_env" "$viewer_before" || true
    note "  (the sha256 of the GET body is compared after the viewer is back)"
    note "  body $(wc -c <"$bench_env" | tr -d ' ') bytes:"
    sed 's/^/    /' "$bench_env"
    "$title_sh" put --dry-run "$bench_env" "$remote_bench_env"
    note "4. install the convert build"
    install_if_needed "convert"
    note "5. stream klog -> $log until /convert: (done, exit|WATCHDOG abandoning|abandoning the process)/ or ${window}s"
    if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
        "$title_sh" launch --dry-run "$package" | sed 's/^/  /'
    else
        note "  launch (no package yet)"
    fi
    note "6. stop, rebuild and reinstall the viewer"
    if [ -d "$package" ] && [ -f "$package/eboot.bin" ]; then
        "$title_sh" stop --dry-run "$package" | sed 's/^/  /'
    else
        note "  stop (no package yet)"
    fi
    note "  make -C $viewer_dir viewer-package"
    install_if_needed "viewer"
    fetch_text "$remote_viewer_env" "$viewer_before" || true
    note "  (a changed viewer.env is put back from the copy saved in step 3)"
    note "dry run: nothing was sent"
    exit 0
fi

: "${PS5_HOST:?convert-inbox: set PS5_HOST or add PS5_HOST=... to .env.local}"

# ---- 1. the convert build ----------------------------------------------------
note "building the convert mode: make -C $bench_dir bench-title"
make -C "$bench_dir" bench-title TITLE_ID="$title_id" || die "the convert build failed (bench-title)" 4
[ -f "$package/eboot.bin" ] || die "no $package/eboot.bin after the build" 4

# ---- 2. inbox ----------------------------------------------------------------
note "inbox $remote_inbox"
ensure_remote_dir "$remote_inbox"
ensure_remote_dir "$data_root/bench"
existing_entries="$(remote_files "$remote_inbox")"
for photo in "${photos[@]+"${photos[@]}"}"; do
    name="$(basename -- "$photo")"
    bytes="$(file_size "$photo")"
    previous="$(printf '%s\n' "$existing_entries" |
        awk -F'\t' -v want="$name" '$1 == want { print $2; exit }')"
    if [ -n "$previous" ]; then
        if [ "$previous" = "$bytes" ]; then
            note "$name: already in the inbox ($bytes bytes), not uploading"
        else
            die "$remote_inbox/$name exists with $previous bytes, local $bytes: the inbox is never overwritten; rename the photo" 2
        fi
        continue
    fi
    note "$name: uploading $(human "$bytes") to $remote_inbox"
    "$title_sh" put "$photo" "$remote_inbox/$name" || die "upload of $name failed" 4
done

# ---- 3. viewer.env first, then the selector ----------------------------------
if fetch_text "$remote_viewer_env" "$viewer_before"; then
    viewer_present_before=1
    viewer_sha_before="$(sha256_of "$viewer_before")"
    note "saved $remote_viewer_env ($(wc -c <"$viewer_before" | tr -d ' ') bytes, sha256 $viewer_sha_before)"
else
    note "no readable $remote_viewer_env on the console; there will be nothing to restore"
fi
note "writing $remote_bench_env (BENCH=convert)"
"$title_sh" put "$bench_env" "$remote_bench_env" || die "writing the selector failed" 4

# ---- 4. install the convert build --------------------------------------------
install_if_needed "convert" || die "the convert build is not installed" 4

# ---- 5. run it to completion -------------------------------------------------
mkdir -p "$out_dir"
note "log $log"
capture_rc=0
PS5_RUN_TAG=convert-inbox PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" \
PS5_RUN_SECONDS="$window" PS5_RUN_OUT="$log" \
PS5_RUN_TITLE_SH="$title_sh" PS5_RUN_PACKAGE="$package" PS5_RUN_KLOG="$klog_sh" \
PS5_RUN_END='(?:convert: (?:done, exit|WATCHDOG abandoning|abandoning the process)|bench_title: convert done, exit)' \
python3 "$capture" || capture_rc=$?

# The title ends itself on every normal path; stop is the quit file for a run
# still inside a photo, and never a kill.
stop_rc=0
"$title_sh" stop "$package" || stop_rc=$?

# ---- 6. the viewer back, and viewer.env as it was ----------------------------
note "rebuilding and reinstalling the viewer: make -C $viewer_dir viewer-package"
make -C "$viewer_dir" viewer-package || die "the viewer build failed; the console still has the convert build" 6
install_files "viewer" || die "reinstalling the viewer failed; the console still has the convert build" 6
"$title_sh" status "$package" >/dev/null || die "the viewer package does not verify on the console" 6
note "viewer reinstalled and verified in $title_id"

viewer_after="$tmp_dir/viewer.env.after"
viewer_sha_after=""
fetch_text "$remote_viewer_env" "$viewer_after" && viewer_sha_after="$(sha256_of "$viewer_after")"
if [ "$viewer_present_before" = 1 ]; then
    if [ "$viewer_sha_after" = "$viewer_sha_before" ]; then
        note "viewer.env unchanged (sha256 $viewer_sha_after)"
    else
        note "viewer.env changed (sha256 ${viewer_sha_after:-absent}); putting the saved copy back"
        "$title_sh" put "$viewer_before" "$remote_viewer_env" ||
            die "restoring viewer.env failed" 6
        fetch_text "$remote_viewer_env" "$viewer_after" &&
            [ "$(sha256_of "$viewer_after")" = "$viewer_sha_before" ] ||
            die "viewer.env still differs from the saved copy after the restore" 6
        note "viewer.env restored (sha256 $viewer_sha_before)"
    fi
elif [ -n "$viewer_sha_after" ]; then
    note "warning: viewer.env did not exist before this run and exists now; no copy to restore, left as it is"
fi

# ---- what the title said -----------------------------------------------------
if [ -f "$log" ]; then
    note "convert lines from the capture:"
    awk '/convert: / { i = index($0, "convert: "); print "  " substr($0, i) }' "$log" >&2
fi

if [ "$capture_rc" -ne 0 ]; then
    note "the convert run did not finish (capture exit $capture_rc); see $log"
    exit "$capture_rc"
fi
if [ "$stop_rc" -ne 0 ]; then
    note "the run finished but the title did not stop (title.sh exit $stop_rc)"
    exit 5
fi
if grep -q 'convert: PASS' "$log" 2>/dev/null; then
    note "PASS: every inbox photo has a scene; the viewer is back in $title_id"
    exit 0
fi
note "FAIL: the title's own verdict was not PASS; see $log"
exit 1
