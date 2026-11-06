#!/usr/bin/env bash
# Stage T's one scripted console check: the Settings "Thumbnail size" slider,
# end to end, with no manual reading.
#
#   PS5_HOST=<dev console ip> platform/ps5/scripts/ui-tests/thumbnail-size-check.sh
#
# It runs thumbnail-size.txt through ui-test.sh, reads the klog and the state
# file, and prints exactly one line:
#
#   thumbnail-size: PASS
#   thumbnail-size: FAIL <reason>
#
# Progress and diagnostics go to stderr, so a caller reads the verdict only.
#
# What it proves numerically:
#   - the slider changes with the pad: the last ten "ui: thumbnail size:" lines
#     are a fixed sequence (after six Lefts pin it to the smallest) through
#     Small, Default, Large, Largest and back to Default, each naming the column
#     count the layout reports (9 at the smallest, 8 at small, 7 at default,
#     6 at large, 5 at the largest);
#   - the value is saved at once: state/ui.json on the console says
#     "thumbnail_size": 2 after the script returns to the default;
#   - the five shots exist (the later visual check's material; the verdict does
#     not read pixels).
#
# The prior ui.json is read first and put back afterwards, so a sitting never
# leaves the user's thumbnail size changed (or a file that did not exist).
#
# Everything goes through the dev identity's data folder (AGENTS.md) and
# ps5-env.sh's guard, so the release install and the user's private data stay
# untouched. It never launches a title itself: ui-test.sh does that only after
# PS5_HOST is set and the dev identity is selected.
#
# Exit: 0 PASS, 1 FAIL (with the reason on the same line).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
scripts="$here"
# shellcheck source=../ps5-env.sh
. "$here/../ps5-env.sh"

UI_TEST="$here/../ui-test.sh"
TITLE="$here/../title.sh"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/thumbnail-size.XXXXXX")" || {
    printf 'thumbnail-size: FAIL cannot create a temporary folder\n'
    exit 1
}

note() { printf 'thumbnail-size: %s\n' "$*" >&2; }

state=""
state_old="$TMP/ui.json.old"
state_had=0
restore_state() {
    [ -n "$state" ] || return 0
    if [ "$state_had" = 1 ]; then
        "$TITLE" put "$state_old" "$state" >/dev/null 2>&1 ||
            note "could not put the old $state back; it holds the run's default size"
    else
        ps5_http POST /api/ftp/delete json "$(ps5_json_path "$state")" >/dev/null 2>&1 || true
    fi
}
fail() {
    restore_state
    rm -rf "$TMP"
    printf 'thumbnail-size: FAIL %s\n' "$*"
    exit 1
}

# expect <log> <grep -E pattern> <what>: the log must hold the line. (A local
# helper, not /usr/bin/expect: the Tcl interpreter would choke on the log and
# its exit status is not the run's.)
expect() {
    grep -Eq -- "$2" "$1" || fail "$3 (no \"$2\" in $(basename "$1"))"
}

[ -n "${PS5_HOST:-}" ] || fail "set PS5_HOST"
[ -f "$UI_TEST" ] || fail "no ui-test.sh at $UI_TEST"

title_id="$(ps5_title_id)" || fail "cannot resolve the build identity"
ps5_agent_guard_identity "$title_id" || fail "identity $title_id is not for agent scripts"
data_root="$(ps5_title_data_dir)" || fail "cannot resolve the data folder"
state="$data_root/state/ui.json"

# Keep what was there before the run (or remember that nothing was).
if ps5_ftp_get_text "$state" "$state_old" 0; then
    state_had=1
    note "saved the prior state/ui.json"
else
    note "no prior state/ui.json"
fi

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
    SHOT_DIR="$dir"
    note "$name log: $LOG"
    return 0
}

# ---- the run -----------------------------------------------------------------
run_script thumbnail-size.txt || fail "the thumbnail-size run did not finish"
expect "$LOG" '^viewer: script done$' "the run did not finish on its own"

# From the smallest (six Lefts pin it whatever the prior ui.json held) the last
# ten steps are fixed: up through Small, Default, Large, Largest, back down to
# Smallest, and two steps to the default. Exact text, exact per-row counts. Stage
# TF2 gives every screen the same column count at every step (9, 8, 7, 6, 5), so
# every line names one number.
cat >"$TMP/expected.txt" <<'EOF'
ui: thumbnail size: Small (8 per row)
ui: thumbnail size: Default (7 per row)
ui: thumbnail size: Large (6 per row)
ui: thumbnail size: Largest (5 per row)
ui: thumbnail size: Large (6 per row)
ui: thumbnail size: Default (7 per row)
ui: thumbnail size: Small (8 per row)
ui: thumbnail size: Smallest (9 per row)
ui: thumbnail size: Small (8 per row)
ui: thumbnail size: Default (7 per row)
EOF
grep '^ui: thumbnail size: ' "$LOG" | tail -n 10 >"$TMP/tail.txt"
if ! diff -u "$TMP/expected.txt" "$TMP/tail.txt" >"$TMP/diff.txt"; then
    cat "$TMP/diff.txt" >&2
    fail "the last ten slider steps are not the script's sequence (see $LOG)"
fi

# ---- the saved value ---------------------------------------------------------
state_copy="$TMP/ui.json"
ps5_ftp_get_text "$state" "$state_copy" 0 || fail "no $state on the console after the run"
grep -q '"thumbnail_size": 2' "$state_copy" ||
    fail "ui.json does not hold the default thumbnail size after the run"
note "ui.json holds the default thumbnail size"

# ---- the shots (the later visual check's material) ---------------------------
count="$(find "$SHOT_DIR" -maxdepth 1 -name 'thumbnail-*.png' | wc -l | tr -d ' ')"
[ "${count:-0}" -ge 5 ] || fail "expected at least 5 thumbnail-*.png shots, found ${count:-0}"
note "$count shots in $SHOT_DIR"

# ---- leave the console as it was --------------------------------------------
restore_state
rm -rf "$TMP"
printf 'thumbnail-size: PASS\n'
exit 0
