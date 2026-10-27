#!/usr/bin/env bash
# Offline check of title-uninstall.sh's ownership decision and of its --data
# rule: no console, no network, nothing is uploaded, unregistered or deleted.
#
#   platform/ps5/title/ownership-test.sh
#
# The decision is identity.py --owns, which title-uninstall.sh calls for the
# param.json that is on the console. This feeds it the installed-paramjson cases
# (the dev build today, the build installed before it, a foreign title on that
# id, the fresh build and a title on PPSA99771 with another name, the release
# build, an id dropped from the table) and then checks that the script refuses
# an id outside the table, and --data on the release identity, before it builds
# any request — and that it accepts the fresh identity's id and its own data
# folder (the run then stops at the missing PS5_HOST, before any request).
# PS5_HOST is unset and .env.local is ignored, so a regression that reached the
# request stage would fail here instead of touching the console.
#
# Exit: 0 every case behaved as documented; 1 one case did not.
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
failures=0

# param_text TITLE_ID TITLE_NAME -> a minimal installed param.json on stdout.
param_text() {
    python3 -c 'import json,sys
print(json.dumps({"titleId": sys.argv[1],
                  "localizedParameters": {"defaultLanguage": "en-US",
                                          "en-US": {"titleName": sys.argv[2]}}}))' "$1" "$2"
}

check_owns() {  # check_owns TITLE_ID TITLE_NAME WANT
    local got
    got="$(param_text "$1" "$2" | python3 "$here/identity.py" --owns "$1")"
    if [ "$got" = "$3" ]; then
        printf '  ok   %s + "%s" -> %s\n' "$1" "$2" "$got"
    else
        printf '  FAIL %s + "%s" -> %s (wanted %s)\n' "$1" "$2" "$got" "$3"
        failures=$((failures + 1))
    fi
}

check_owns PPSA99202 "Photo Splat VR Dev" yes     # the dev build today
check_owns PPSA99202 "PS5 Splat VR Viewer" yes   # the build installed before it
check_owns PPSA99202 "Twiso" no                  # foreign title on our id
check_owns PPSA99202 "Photo Splat VR" no         # the release identity's name
check_owns PPSA99771 "Photo Splat VR Fresh Test" yes  # the fresh build
check_owns PPSA99771 "Photo Splat VR" no         # another identity's name on our fresh id
check_owns PPSA99771 "Photo Splat VR Fresh" no   # a prefix of the fresh name is not the name
check_owns PPSA99771 "Twiso" no                  # a foreign title on the fresh id
check_owns PPSA99770 "Photo Splat VR" yes        # the release build
check_owns PPSA99770 "Photo Splat VR Dev" no     # the dev identity's name
check_owns PPSA99770 "Photo Splat VR Fresh Test" no  # the fresh identity's name
check_owns PPSA99202 "Photo Splat VR Fresh Test" no  # the fresh name on the dev id
check_owns PPSA99203 "Photo Splat VR" no         # an id the table dropped

# An id outside the table, and --data on the release identity, are refused
# before any request: PS5_HOST is unset and .env.local is not read, so the
# refusal must come from the table checks (a regression that got past them
# would stop at "set PS5_HOST" instead, which this counts as a failure).
check_refused() {  # check_refused EXPECTED_MESSAGE ARGUMENT...
    local want="$1" out rc
    shift
    out="$(env -u PS5_HOST -u IDENTITY PS5_ENV_FILE=/dev/null \
        "$repo/platform/ps5/scripts/title-uninstall.sh" "$@" 2>&1)"
    rc=$?
    if [ "$rc" = 2 ] && printf '%s' "$out" | grep -qF -- "$want"; then
        printf '  ok   title-uninstall.sh %s -> exit 2: %s\n' "$*" "$want"
    else
        printf '  FAIL title-uninstall.sh %s -> exit %s\n%s\n' "$*" "$rc" "$out"
        failures=$((failures + 1))
    fi
}

check_refused "refusing PPSA99203: it is not in platform/ps5/title/identities.json" PPSA99203
check_refused "refusing --data PPSA99770:" --data PPSA99770
check_refused "refusing PPSA99203: it is not in platform/ps5/title/identities.json" \
    PPSA99203 --data

# The fresh identity is ours: its id passes the allowlist and --data may name its
# own folder. The run then stops at the missing PS5_HOST, i.e. before any
# request — the opposite of the refusals above. A title on PPSA99771 with a
# different titleName is refused by the ownership proof, which is the same
# identity.py --owns the cases at the top of this file exercise.
check_reaches_console() {  # check_reaches_console ARGUMENT...
    local out rc
    out="$(env -u PS5_HOST -u IDENTITY PS5_ENV_FILE=/dev/null \
        "$repo/platform/ps5/scripts/title-uninstall.sh" "$@" 2>&1)"
    rc=$?
    if [ "$rc" = 2 ] && printf '%s' "$out" | grep -qF -- "set PS5_HOST"; then
        printf '  ok   title-uninstall.sh %s -> table and --data checks passed\n' "$*"
    else
        printf '  FAIL title-uninstall.sh %s -> exit %s\n%s\n' "$*" "$rc" "$out"
        failures=$((failures + 1))
    fi
}

check_reaches_console PPSA99771
check_reaches_console --data PPSA99771

if [ "$failures" -ne 0 ]; then
    printf 'ownership-test: FAIL (%d case(s))\n' "$failures"
    exit 1
fi
printf 'ownership-test: PASS — 13 ownership cases, 3 pre-request refusals, 2 fresh allowlist/--data cases; no console was contacted\n'
