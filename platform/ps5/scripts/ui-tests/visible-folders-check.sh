#!/usr/bin/env bash
# Stage H/U1's one scripted console check: the "Visible folders" passcode and
# scene-folder allow-list, end to end, with no manual reading.
#
#   PS5_HOST=<ip> platform/ps5/scripts/ui-tests/visible-folders-check.sh
#
# Stage U1 (user decision 2026-11-06) changed the scope: the list hides scene
# folders only, so the photo picker must stay open and the network location list
# must show every configured share. The checks below assert exactly that, plus
# the original family-b/scene-b absence in the setup and locked logs.
#
# Preconditions: the scenes folder below holds two converted scene folders, and
# the console has at least one network location configured (Settings > Network
# locations). The check creates the scenes itself; it never creates a share, so
# a console without one fails with that reason instead of blaming the list.
#
# It creates two converted scene folders on the dev console (family-a/scene-a
# and family-b/scene-b) from $SPLAT_DATA_DIR/runs/v0.2-library/console/scenes,
# runs the three TV input scripts through ui-test.sh (setup, locked, off),
# checks the klog and the state file, deletes the state file and prints exactly
# one line:
#
#   visible-folders: PASS
#   visible-folders: FAIL <reason>
#
# Progress and diagnostics go to stderr, so a caller reads the verdict only.
#
# The passcode is 2468. Its string must never reach the log or the state file,
# and family-b / scene-b must never reach the setup or locked log: those are the
# two leaks the feature exists to prevent. The off run opens family-b/scene-b
# again, which is the positive proof that turning the passcode off restores the
# gallery.
#
# Everything goes through the dev identity's data folder (AGENTS.md), and every
# request goes through ps5-env.sh's guard, so the release install and the user's
# private data stay untouched.
#
# Exit: 0 PASS, 1 FAIL (with the reason on the same line).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
scripts="$here"
repo="$(cd -- "$here/../../../.." && pwd)"
# shellcheck source=../ps5-env.sh
. "$here/../ps5-env.sh"

PASSCODE=2468
UI_TEST="$here/../ui-test.sh"
TITLE="$here/../title.sh"
FIXTURES="${SPLAT_DATA_DIR:-$repo/..}/runs/v0.2-library/console/scenes"
state=""
TMP="$(mktemp -d "${TMPDIR:-/tmp}/visible-folders.XXXXXX")" || {
    printf 'visible-folders: FAIL cannot create a temporary folder\n'
    exit 1
}

# One line on stdout (the verdict); progress and diagnostics on stderr.
note() { printf 'visible-folders: %s\n' "$*" >&2; }
fail() {
    if [ -n "$state" ]; then
        # A check must not leave a passcode on the console.
        ps5_http POST /api/ftp/delete json "$(ps5_json_path "$state")" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP"
    printf 'visible-folders: FAIL %s\n' "$*"
    exit 1
}

[ -n "${PS5_HOST:-}" ] || fail "set PS5_HOST"
[ -f "$UI_TEST" ] || fail "no ui-test.sh at $UI_TEST"

title_id="$(ps5_title_id)" || fail "cannot resolve the build identity"
ps5_agent_guard_identity "$title_id" || fail "identity $title_id is not for agent scripts"
data_root="$(ps5_title_data_dir)" || fail "cannot resolve the data folder"
scenes="$data_root/scenes"
state="$data_root/state/visible-folders.json"

# ---- remote helpers (GET /api/ftp/list, POST /api/ftp/mkdir, title.sh put) ----
remote_has() {  # remote_has <dir> -> 0 when the folder lists
    ps5_ftp_list_checked "$1" 0 >/dev/null 2>&1
}

remote_mkdir() {  # remote_mkdir <dir>: mkdir -p, one level at a time
    local rest="${1#/}" prefix="" part
    local IFS='/'
    for part in $rest; do
        prefix="$prefix/$part"
        remote_has "$prefix" && continue
        ps5_http POST /api/ftp/mkdir json "$(ps5_json_path "$prefix")" >/dev/null || return 1
    done
    return 0
}

# ---- the fixture -------------------------------------------------------------
converted_scenes() {  # one path per converted scene folder under $1
    local dir
    for dir in "$1"/*/; do
        [ -d "$dir" ] || continue
        [ -f "$dir/scene.psplat" ] && [ -f "$dir/meta.json" ] || continue
        grep -q '"origin"' "$dir/meta.json" 2>/dev/null && continue
        printf '%s\n' "${dir%/}"
    done
}

converted_scenes "$FIXTURES" >"$TMP/scenes.txt"
scene_count="$(wc -l <"$TMP/scenes.txt" | tr -d ' ')"
if [ "${scene_count:-0}" -lt 2 ]; then
    fail "need two converted scene folders (no \"origin\" in meta.json) under $FIXTURES, found ${scene_count:-0}"
fi
source_a="$(sed -n 1p "$TMP/scenes.txt")"
source_b="$(sed -n 2p "$TMP/scenes.txt")"
note "fixture: $source_a and $source_b"

put_scene() {  # put_scene <source dir> <remote album> <remote scene name>
    local source="$1" album="$2" name="$3" file remote
    remote="$scenes/$album/$name"
    remote_mkdir "$remote" || fail "cannot create $remote on the console"
    for file in scene.psplat meta.json thumb.jpg original.jpg; do
        [ -f "$source/$file" ] || continue
        "$TITLE" put "$source/$file" "$remote/$file" >/dev/null 2>&1 ||
            fail "cannot upload $source/$file to $remote/$file"
    done
}

put_scene "$source_a" family-a scene-a
put_scene "$source_b" family-b scene-b

# ---- one ui-test.sh run ------------------------------------------------------
LOG=""
run_script() {  # run_script <name>: sets LOG to that run's viewer.log
    local name="$1" out="$TMP/$name.out" err="$TMP/$name.err" dir
    : >"$out"
    : >"$err"
    note "running $name"
    if ! bash "$UI_TEST" "$scripts/$name" >"$out" 2>"$err"; then
        note "ui-test.sh failed for $name:"
        tail -n 6 "$err" >&2
        return 1
    fi
    # ui-test.sh prints each pulled shot; their folder holds viewer.log too.
    dir="$(head -n 1 "$out" | sed 's|/[^/]*$||')"
    if [ -z "$dir" ] || [ ! -f "$dir/viewer.log" ]; then
        dir="$(sed -n 's/^ui-test: PASS: [0-9]* shot(s) in \(.*\)$/\1/p' "$err" | tail -n 1)"
    fi
    [ -n "$dir" ] && [ -f "$dir/viewer.log" ] || return 1
    LOG="$dir/viewer.log"
    note "$name log: $LOG"
    return 0
}

expect() {  # expect <log> <grep -E pattern> <what>
    grep -Eq -- "$2" "$1" || fail "$3 (no \"$2\" in $(basename "$1"))"
}

expect_absent() {  # expect_absent <log> <grep -c pattern> <what>
    local count
    count="$(grep -Ec -- "$2" "$1")"
    [ "$count" -eq 0 ] || fail "$3 ($count line(s) matching \"$2\" in $(basename "$1"))"
}

# ---- 1. the setup run: set the passcode, tick one album ----------------------
run_script visible-folders-setup.txt || fail "the setup run did not finish"
SETUP_LOG="$LOG"
expect "$SETUP_LOG" '^viewer: tv scene 1/1 ' "the setup did not open a scene"
expect "$SETUP_LOG" '^viewer: hidden scene closed' "setting the passcode did not close the open scene"
expect "$SETUP_LOG" '^ui: visible folders: passcode set' "the passcode was not set"
expect "$SETUP_LOG" '^ui: visible folders: passcode on; 1 of ' "the tick was not saved"
expect "$SETUP_LOG" '^ui: toast Passcode set\. Tick the folders that may be seen\.$' \
    "the passcode-set toast is missing"
# The picker is not filtered (stage U1): it must still list its sources, and the
# old "0 sources visible" line no longer exists.
expect "$SETUP_LOG" '^ui: sources .*inbox' "the photo picker did not list the Inbox"
# The removed filter line itself ("ui: photos: N sources visible"), not the
# shared "ui: photos: " prefix: the picker logs its listing errors with that
# prefix too, and an unreadable folder is not a filtered picker.
expect_absent "$SETUP_LOG" '^ui: photos: [0-9]+ sources visible$' \
    "the photo picker still filters its sources"
expect_absent "$SETUP_LOG" "$PASSCODE" "the passcode string reached the setup log"
expect_absent "$SETUP_LOG" 'family-b' "a hidden album name reached the setup log"
expect_absent "$SETUP_LOG" 'scene-b' "a hidden scene name reached the setup log"

# ---- 2. the state file, before the off run deletes it -----------------------
state_copy="$TMP/visible-folders.json"
ps5_ftp_get_text "$state" "$state_copy" 0 || fail "no $state on the console after the setup run"
grep -q '"algorithm": "pbkdf2-hmac-sha256"' "$state_copy" ||
    fail "the state file does not name the hash algorithm"
grep -q '"salt": "[0-9a-f]\{32\}"' "$state_copy" || fail "the state file has no 32-hex salt"
grep -q '"hash": "[0-9a-f]\{64\}"' "$state_copy" || fail "the state file has no 64-hex hash"
grep -q 'album:family-a' "$state_copy" || fail "the state file does not hold the ticked album"
if grep -q "$PASSCODE" "$state_copy"; then
    fail "the passcode string is in the state file"
fi
# The console's FTP listing does not report a mode; the desktop test proves 0600
# (tests/visibility_test.cpp, "visibility file is 0600 on POSIX").
note "state file: hash and salt present, no passcode string (0600 proved by the desktop test)"

# ---- 3. the locked run: wrong then right passcode ---------------------------
run_script visible-folders-locked.txt || fail "the locked run did not finish"
LOCKED_LOG="$LOG"
expect "$LOCKED_LOG" '^ui: albums: 1 visible, ' "the relaunch did not list exactly one visible album"
expect "$LOCKED_LOG" '^ui: toast Wrong passcode$' "a wrong passcode was not reported"
expect "$LOCKED_LOG" '^ui: visible folders: wrong passcode$' "a wrong passcode was not logged"
expect "$LOCKED_LOG" '^ui: visible folders: passcode on; 1 of ' "the right passcode did not unlock"
expect "$LOCKED_LOG" '^ui: sources .*inbox' "the photo picker did not list the Inbox"
# Settings > Network locations lists every configured share, passcode or not.
# The check does not create a share (see the preconditions at the top), so the
# two failures below name which of the two things is wrong.
if ! grep -Eq -- '^ui: locations: [1-9][0-9]* configured$' "$LOCKED_LOG"; then
    if grep -Eq -- '^ui: locations: 0 configured$' "$LOCKED_LOG"; then
        fail "no network location is configured on the console; add one in Settings > Network locations first"
    fi
    fail "the location list did not open in the locked run (no \"ui: locations: N configured\" line)"
fi
expect_absent "$LOCKED_LOG" '^ui: photos: [0-9]+ sources visible$' \
    "the photo picker still filters its sources"
expect_absent "$LOCKED_LOG" "$PASSCODE" "the passcode string reached the locked log"
expect_absent "$LOCKED_LOG" 'family-b' "a hidden album name reached the locked log"
expect_absent "$LOCKED_LOG" 'scene-b' "a hidden scene name reached the locked log"

# ---- 4. the off run: every folder back --------------------------------------
run_script visible-folders-off.txt || fail "the off run did not finish"
OFF_LOG="$LOG"
expect "$OFF_LOG" '^ui: visible folders: passcode off$' "the passcode was not turned off"
expect "$OFF_LOG" '^viewer: tv scene 1/1 scene-b \(album family-b\)$' \
    "the hidden album/scene did not come back after turning the passcode off"
expect_absent "$OFF_LOG" "$PASSCODE" "the passcode string reached the off log"

if ps5_ftp_list_checked "$data_root/state" 0 |
    awk -F'\t' '$1 == "visible-folders.json" { found = 1 } END { exit !found }'; then
    fail "the state file still exists after turning the passcode off"
fi

# ---- 5. leave nothing behind ------------------------------------------------
note "PASS"
rm -rf "$TMP"
printf 'visible-folders: PASS\n'
exit 0
