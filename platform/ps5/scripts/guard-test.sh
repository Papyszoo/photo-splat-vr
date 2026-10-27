#!/usr/bin/env bash
# Offline check of the agent guards: no console, no network, nothing is read,
# listed, written, launched or deleted.
#
#   platform/ps5/scripts/guard-test.sh
#
# User decision (AGENTS.md): three installs live on the console. Agent scripts
# may drive the dev and the fresh identity and never the release one, and no
# script in this repo may read, list, write or delete under the release
# identity's data folder (the user's private data). Both facts come from
# platform/ps5/title/identities.json: "agentsMayRun" and each identity's
# dataDir. This test runs every console script against the release identity and
# against the private data path and requires a refusal that happens before any
# request: PS5_HOST is unset, so a regression that reached the request stage
# would stop at "set PS5_HOST" (a different message) instead. It also checks
# that the same scripts still plan a dev run and a fresh run (the guards are not
# a blanket refusal), that installing the release package files is still
# allowed (only launching it and touching its data are refused), and that
# IDENTITY selects an identity by name while an unknown name or RELEASE=1
# beside another name is refused.
#
# Last, a mock package with the bundled sample names ("Living room",
# "York street") and shell/URL metacharacters must install: every upload and
# mkdir request is printed with a percent-encoded path (no raw space in the
# request target), and a remote path with a control character is still refused.
#
# Exit: 0 every case behaved as documented; 1 one case did not.
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
title_script="$here/../title"
failures=0
refusals=0
allowed=0
plans=0
private_dir="/data/photo-splat-vr"

say() { printf 'guard-test: %s\n' "$*" >&2; }

# run_offline ENV... -- COMMAND...: run a script with no console configured and
# .env.local ignored, capture output and exit code.
run_offline() {
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    output="$(env -u PS5_HOST -u RELEASE -u IDENTITY PS5_ENV_FILE=/dev/null "${envs[@]}" \
        "$@" 2>&1)"
    status=$?
}

check_refused() {  # check_refused LABEL MESSAGE -- ENV... -- COMMAND...
    local label="$1" message="$2"
    shift 2
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    run_offline "${envs[@]}" -- "$@"
    if [ "$status" -ne 2 ]; then
        printf '  FAIL %-52s exit %s (wanted 2)\n%s\n' "$label" "$status" "$output"
        failures=$((failures + 1))
        return
    fi
    if ! printf '%s' "$output" | grep -qF -- "$message"; then
        printf '  FAIL %-52s exit 2 but not the guard message\n%s\n' "$label" "$output"
        failures=$((failures + 1))
        return
    fi
    if printf '%s' "$output" | grep -qE '^ *\$? ?(GET|POST) '; then
        printf '  FAIL %-52s printed a request before refusing\n%s\n' "$label" "$output"
        failures=$((failures + 1))
        return
    fi
    printf '  ok   %-52s exit 2 before any request\n' "$label"
    refusals=$((refusals + 1))
}

check_allowed() {  # check_allowed LABEL MESSAGE -- ENV... -- COMMAND...
    local label="$1" message="$2"
    shift 2
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    run_offline "${envs[@]}" -- "$@"
    if [ "$status" -eq 0 ] && printf '%s' "$output" | grep -qF -- "$message"; then
        printf '  ok   %-52s exit 0, plan printed\n' "$label"
        allowed=$((allowed + 1))
    else
        printf '  FAIL %-52s exit %s\n%s\n' "$label" "$status" "$output"
        failures=$((failures + 1))
    fi
}

check_permitted() {  # check_permitted LABEL MESSAGE -- ENV... -- COMMAND...
    # Like check_allowed, for a call that prints no plan of its own (a guard
    # function): exit 0 and, when MESSAGE is not empty, that text.
    local label="$1" message="$2"
    shift 2
    local envs=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    shift
    run_offline "${envs[@]}" -- "$@"
    if [ "$status" -eq 0 ] &&
       { [ -z "$message" ] || printf '%s' "$output" | grep -qF -- "$message"; }; then
        printf '  ok   %-52s exit 0, allowed\n' "$label"
        allowed=$((allowed + 1))
    else
        printf '  FAIL %-52s exit %s (wanted 0)\n%s\n' "$label" "$status" "$output"
        failures=$((failures + 1))
    fi
}

guard_message="agent scripts never launch the release install"
private_message="is the user's private data folder"
absolute_message="must be absolute"
empty_message="an empty console path"

# ---- a fake release package (the scripts only read its param.json/eboot) -----
tmp="$(mktemp -d "${TMPDIR:-/tmp}/ps5splat-guard.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
release_package="$tmp/PPSA99770"
mkdir -p "$release_package/sce_sys"
printf 'not an eboot\n' >"$release_package/eboot.bin"
python3 "$title_script/identity.py" --param "$title_script/param.json" \
    --out "$release_package/sce_sys/param.json" --title-id PPSA99770
# The same fake for the dev identity: run-viewer.sh and title.sh want a package
# before they print their plan, and this test must not depend on a build (or on
# whatever the real package happens to carry today).
fresh_package="$tmp/PPSA99771"
mkdir -p "$fresh_package/sce_sys" "$fresh_package/shaders"
printf 'not an eboot\n' >"$fresh_package/eboot.bin"
printf 'not a shader\n' >"$fresh_package/shaders/project.comp.spv"
python3 "$title_script/identity.py" --param "$title_script/param.json" \
    --out "$fresh_package/sce_sys/param.json" --title-id PPSA99771
dev_package="$tmp/PPSA99202"
mkdir -p "$dev_package/sce_sys" "$dev_package/shaders"
printf 'not an eboot\n' >"$dev_package/eboot.bin"
printf 'not a shader\n' >"$dev_package/shaders/project.comp.spv"
python3 "$title_script/identity.py" --param "$title_script/param.json" \
    --out "$dev_package/sce_sys/param.json" --title-id PPSA99202
printf 'quit\n' >"$tmp/quit.txt"

say "release identity (PPSA99770) is refused before any request"
check_refused "push-data.sh --check" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/push-data.sh" --check
check_refused "convert-inbox.sh --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/convert-inbox.sh" --dry-run
check_refused "run-viewer.sh --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/run-viewer.sh" --dry-run
check_refused "run-vr-hello.sh --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/run-vr-hello.sh" --dry-run
check_refused "run-bench.sh --title --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/../bench/run-bench.sh" --title --dry-run op_audit
check_refused "ui-test.sh --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/ui-test.sh" "$here/ui-tests/browse.txt" --dry-run
check_refused "spike.sh --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/spike.sh" --dry-run
check_refused "title.sh launch --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/title.sh" launch --dry-run "$release_package"
check_refused "title.sh stop --dry-run" "$guard_message" \
    TITLE_ID=PPSA99770 -- "$here/title.sh" stop --dry-run "$release_package"

say "the release data folder is refused as a path, whatever the identity"
check_refused "title.sh put /data/photo-splat-vr/..." "$private_message" \
    TITLE_ID=PPSA99202 -- "$here/title.sh" put --dry-run "$tmp/quit.txt" \
    "$private_dir/title-quit"
check_refused "push-data.sh --check PUSH_REMOTE_ROOT=private" "$private_message" \
    TITLE_ID=PPSA99202 PUSH_REMOTE_ROOT="$private_dir" -- "$here/push-data.sh" --check
check_refused "ps5_guard_remote_path private dir" "$private_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2/x"' _ "$here" "$private_dir"
check_refused "ps5_guard_remote_path private dir itself" "$private_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2"' _ "$here" "$private_dir"
check_refused "ps5_agent_guard_identity PPSA99770" "$guard_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_agent_guard_identity PPSA99770' _ "$here"

say "the dev identity still plans a run, and installing the release files stays allowed"
check_allowed "title.sh install --dry-run release package" "install PPSA99770 -> /data/homebrew/PPSA99770" \
    -- "$here/title.sh" install --dry-run "$release_package"
check_allowed "title.sh status --dry-run release package" "status PPSA99770" \
    -- "$here/title.sh" status --dry-run "$release_package"
check_allowed "title.sh install --dry-run dev package" "install PPSA99202 -> /data/homebrew/PPSA99202" \
    -- "$here/title.sh" install --dry-run "$dev_package"
check_allowed "run-viewer.sh --dry-run (dev)" "title route: PPSA99202" \
    -- "$here/run-viewer.sh" --dry-run --package "$dev_package"

say "the fresh identity (IDENTITY=fresh) builds, installs, launches and is uninstalled like dev"
check_allowed "title.sh install --dry-run fresh package" "install PPSA99771 -> /data/homebrew/PPSA99771" \
    IDENTITY=fresh -- "$here/title.sh" install --dry-run "$fresh_package"
check_allowed "title.sh launch --dry-run fresh package" "launch PPSA99771" \
    IDENTITY=fresh -- "$here/title.sh" launch --dry-run "$fresh_package"
check_allowed "title.sh stop --dry-run fresh package" "stop PPSA99771" \
    IDENTITY=fresh -- "$here/title.sh" stop --dry-run "$fresh_package"
check_allowed "title.sh put --dry-run fresh data dir" "-> /data/psvr-fresh-test/title-quit" \
    IDENTITY=fresh -- "$here/title.sh" put --dry-run "$tmp/quit.txt" \
    "/data/psvr-fresh-test/title-quit"
check_allowed "run-viewer.sh --dry-run (fresh)" "title route: PPSA99771" \
    IDENTITY=fresh -- "$here/run-viewer.sh" --dry-run --package "$fresh_package"
check_permitted "ps5_agent_guard_identity PPSA99771" "" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_agent_guard_identity PPSA99771' _ "$here"
check_permitted "ps5_guard_remote_path /data/psvr-fresh-test/x" "" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2/x"' _ "$here" /data/psvr-fresh-test
check_permitted "ps5_guard_remote_path /data/photo-splat-vr-x/x" "" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2/x"' _ "$here" /data/photo-splat-vr-x

say "the fresh identity refuses what it always refused"
# An identity is not a key to the release install: the guard reads the package's
# title id, so IDENTITY=fresh changes nothing about PPSA99770.
check_refused "title.sh launch release package, IDENTITY=fresh" "$guard_message" \
    IDENTITY=fresh -- "$here/title.sh" launch --dry-run "$release_package"
check_refused "title.sh put private path, IDENTITY=fresh" "$private_message" \
    IDENTITY=fresh -- "$here/title.sh" put --dry-run "$tmp/quit.txt" \
    "$private_dir/title-quit"
check_refused "ps5_guard_remote_path private dir, IDENTITY=fresh" "$private_message" \
    IDENTITY=fresh -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2/x"' _ "$here" "$private_dir"
# Component-wise comparison, not a string prefix: "." collapses to the private
# folder and a ".." is refused rather than resolved.
check_refused "ps5_guard_remote_path /data/./photo-splat-vr" "$private_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2/x"' _ "$here" "/data/./photo-splat-vr"
check_refused "ps5_guard_remote_path /data/photo-splat-vr/../x" "a '..' component" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2"' _ "$here" "$private_dir/../x"
check_refused "ps5_guard_remote_path //data//photo-splat-vr" "$private_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "$2"' _ "$here" "//data//photo-splat-vr"
# A path the whole-component check cannot compare is refused, never let through:
# a relative path never matches an absolute private folder, and an empty one
# names nothing. ".." is refused before it can be resolved to the private folder.
check_refused "ps5_guard_remote_path relative data/photo-splat-vr" "$absolute_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "data/photo-splat-vr"' _ "$here"
check_refused "ps5_guard_remote_path relative photo-splat-vr/x" "$absolute_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "photo-splat-vr/x"' _ "$here"
check_refused "ps5_guard_remote_path (empty)" "$empty_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path ""' _ "$here"
check_refused "ps5_guard_remote_path /data/../data/photo-splat-vr" "a '..' component" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "/data/../data/photo-splat-vr"' _ "$here"
check_refused "ps5_guard_remote_path //data/photo-splat-vr" "$private_message" \
    -- bash -c '. "$1/ps5-env.sh"; ps5_guard_remote_path "//data/photo-splat-vr"' _ "$here"
# IDENTITY itself: a name the table does not hold, and RELEASE=1 beside another
# name, stop before any request is built.
check_refused "IDENTITY=<unknown name>" "unknown identity 'nope'" \
    IDENTITY=nope -- "$here/run-viewer.sh" --dry-run --package "$dev_package"
check_refused "RELEASE=1 with IDENTITY=fresh" "asks for another one" \
    IDENTITY=fresh RELEASE=1 -- "$here/run-viewer.sh" --dry-run --package "$dev_package"
check_refused "RELEASE=1 with IDENTITY=dev" "asks for another one" \
    IDENTITY=dev RELEASE=1 -- "$here/title.sh" put --dry-run "$tmp/quit.txt" \
    "/data/ps5-splat-vr/title-quit"

# ---- install plans: spaces and shell/URL metacharacters ---------------------
# The bundled sample folders are "Living room", "Sailboats" and "York street"
# ($SPLAT_DATA_DIR/assets/samples/out). A console path may carry a space — the
# ProsperoMgr file API takes it percent-encoded — so the install plan must quote
# it in the shell and encode it in each request instead of refusing it.
enc_package="$tmp/enc/PPSA99202"
mkdir -p "$enc_package/sce_sys" "$enc_package/samples/Living room" \
    "$enc_package/samples/York street" "$enc_package/samples/It's #1 & 100% (v2)+?"
printf 'not an eboot\n' >"$enc_package/eboot.bin"
python3 "$title_script/identity.py" --param "$title_script/param.json" \
    --out "$enc_package/sce_sys/param.json" --title-id PPSA99202
printf 'credits\n' >"$enc_package/samples/CREDITS.md"
for scene in "Living room" "York street" "It's #1 & 100% (v2)+?"; do
    printf 'meta\n' >"$enc_package/samples/$scene/meta.json"
    printf 'psplat\n' >"$enc_package/samples/$scene/scene.psplat"
done
# A file name with every shell/URL character that used to break a request target.
printf 'special\n' >"$enc_package/samples/Living room/a+b&c #1 (2)?.jpg"

# check_install_plan LABEL PACKAGE: the printed dry-run plan carries one encoded
# upload per local file and one encoded listing/mkdir per local directory, and
# decoding the encoded path gives back the exact remote path.
check_install_plan() {
    local label="$1" package="$2" report
    run_offline -- "$here/title.sh" install --dry-run "$package"
    if [ "$status" -ne 0 ]; then
        printf '  FAIL %-52s exit %s\n%s\n' "$label" "$status" "$output"
        failures=$((failures + 1))
        return
    fi
    printf '%s\n' "$output" >"$tmp/plan.out"
    if ! report="$(python3 - "$package" "$tmp/plan.out" <<'PY'
import json, os, re, sys, urllib.parse

package, plan = sys.argv[1], sys.argv[2]
root = "/data/homebrew/" + os.path.basename(package)
problems = []

files = {}
for directory, _, names in os.walk(package):
    for name in names:
        full = os.path.join(directory, name)
        files[os.path.relpath(full, package).replace(os.sep, "/")] = os.path.getsize(full)
# The install creates /data/homebrew and then one directory per subdirectory of
# the package (the package root itself is not created here; the upload route
# makes it, see docs/BACKLOG.md).
dirs = {"/data/homebrew"}
for directory, subdirs, _ in os.walk(package):
    for name in subdirs:
        dirs.add(root + "/" + os.path.relpath(os.path.join(directory, name), package)
                 .replace(os.sep, "/"))

uploads = {}
mkdirs = []
lists = []
for line in open(plan, encoding="utf-8"):
    line = line.rstrip("\n")
    match = re.match(r"^  POST (\S+)/api/ftp/upload\?path=([^&]+)&overwrite=1  body (\d+) bytes \(", line)
    if match:
        encoded = match.group(2)
        uploads[urllib.parse.unquote(encoded)] = (encoded, int(match.group(3)))
        continue
    match = re.match(r"^  POST \S+/api/ftp/mkdir  body (\{.*\})(?:\s+\(.*\))?$", line)
    if match:
        mkdirs.append(json.loads(match.group(1)).get("path"))
        continue
    match = re.match(r"^  GET \S+/api/ftp/list\?path=([^&\s]+)", line)
    if match:
        lists.append(urllib.parse.unquote(match.group(1)))

wanted = {root + "/" + relative: size for relative, size in files.items()}
for remote, size in sorted(wanted.items()):
    if remote not in uploads:
        problems.append("no upload request for %r" % remote)
        continue
    encoded, sent = uploads[remote]
    if encoded != urllib.parse.quote(remote, safe=""):
        problems.append("%r encoded as %r, wanted %r"
                        % (remote, encoded, urllib.parse.quote(remote, safe="")))
    if sent != size:
        problems.append("%r sends %d bytes, local file is %d" % (remote, sent, size))
for remote in sorted(set(uploads) - set(wanted)):
    problems.append("upload request for %r, which is not in the package" % remote)

wanted_dirs = dirs
listed = set(lists)
for directory in sorted(wanted_dirs):
    if directory not in listed:
        problems.append("no listing of %r" % directory)
    if directory not in mkdirs:
        problems.append("no mkdir request for %r" % directory)
for directory in sorted(set(mkdirs) - wanted_dirs):
    problems.append("mkdir request for %r, which is not in the package" % directory)

if problems:
    sys.exit("\n".join(problems))
print("%d files, %d dirs, every path percent-encoded and reversible"
      % (len(files), len(wanted_dirs)))
PY
    )"; then
        printf '  FAIL %-52s %s\n' "$label" "$report"
        failures=$((failures + 1))
        return
    fi
    printf '  ok   %-52s %s\n' "$label" "$report"
    plans=$((plans + 1))
}

say "a package with spaces and metacharacters plans a correct install"
check_install_plan "install --dry-run, mock package" "$enc_package"
if [ -f "$repo/build/ps5/title/PPSA99202/sce_sys/param.json" ] &&
   [ -d "$repo/build/ps5/title/PPSA99202/samples" ]; then
    check_install_plan "install --dry-run, build/ps5/title/PPSA99202" \
        "$repo/build/ps5/title/PPSA99202"
else
    printf '  skip %-52s (no build; make -C platform/ps5/viewer)\n' \
        "build/ps5/title/PPSA99202"
fi
# The remaining refusal is not dropped by the fix: a path with a control
# character still stops before the first request.
check_refused "title.sh put path with a control character" "has a control character" \
    -- "$here/title.sh" put --dry-run "$tmp/quit.txt" "/data/ps5-splat-vr/bad"$'\t'"name"

if [ "$failures" -ne 0 ]; then
    printf 'guard-test: FAIL (%d case(s))\n' "$failures"
    exit 1
fi
printf 'guard-test: PASS — %d refusal cases, %d allowed cases, %d install plans with spaces/metacharacters; no console was contacted\n' \
       "$refusals" "$allowed" "$plans"
