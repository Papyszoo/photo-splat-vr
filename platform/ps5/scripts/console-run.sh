#!/usr/bin/env bash
# The unattended console pass: one command runs every scripted check, asserts
# each from the klog (and from the shots' pixels), resumes where it stopped and
# skips the cases that already passed.
#
#   platform/ps5/scripts/console-run.sh --list
#   platform/ps5/scripts/console-run.sh --dry-run
#   platform/ps5/scripts/console-run.sh                 # run the cases not yet passed
#   platform/ps5/scripts/console-run.sh --only net-probe
#   platform/ps5/scripts/console-run.sh --only smb-library,smb-upload --force
#   platform/ps5/scripts/console-run.sh --with-fresh-identity
#   platform/ps5/scripts/console-run.sh --replay DIR     # assertions only, on saved logs
#
# `net_probe` runs first and proves the console can reach a share at all (SMB
# and NFS by IP, no scene, under three minutes); every case that needs a network
# location refuses to run until it has passed in this sitting, so an hour of
# ambiguous failures cannot follow an unreachable share (console pass 3, T2).
#
# `fresh_identity` is NOT part of a default run: it installs and uninstalls a
# title identity, which agents may not do implicitly (AGENTS.md). It runs only
# with --with-fresh-identity and is otherwise reported as SKIP.
#
# Why it exists: CONSOLE-TEST.md is a checklist a person reads. Every item that
# is not "look at the screen" is a log line or a pixel count, so the pass can be
# driven by the console itself: ui-test.sh runs one script, this script decides
# pass or fail from the resulting klog (`viewer: ...`) and the shots, one case
# after another, and writes a marker for each pass. A second sitting continues
# at the first failing/unrun case.
#
# A case is a bash function (see "the cases" below) that fills:
#   CASE_SCRIPT       the ui-tests/*.txt to run
#   CASE_EXPECT       one regex per line the klog must contain, in any order
#   CASE_REJECT       one regex per line the klog must not contain
#   CASE_SHOTS        "<shot>:<min content fraction>[:<max fraction>]" - the PNG
#                     must have at least that fraction of pixels that are not
#                     the backdrop (and at most the max, when given, so a UI
#                     screen cannot stand in for a scene)
#   CASE_LOCATIONS    a locations.json to push before the run ("smb", "nfs", ...)
#   CASE_SERVERS      test servers to start here first ("smb-user", "nfs", ...)
#   CASE_STOP_SERVER  1 = start the servers, then stop them before the run
#   CASE_DATA         a folder to push into the console inbox ("" = none)
#   CASE_INBOX_NAMES  a function that renames each pushed file ("0a-big.splat"),
#                     so the picker's cell order is the case's, not the inbox's
#   CASE_FREE_SPACE   MiB the console's free space may lose during the run
#                     (0 = not checked); the save-to-share case uses it as the
#                     "the staging copy is really gone" check
#   CASE_ENV          extra viewer.env lines (KEY=VALUE)
#   CASE_SECONDS      the run's length (0 = from the script)
#   CASE_PRE          helper steps before the run (may append to CASE_EXPECT)
#   CASE_CHECK        helper steps after it (each must return 0)
#   CASE_NEEDS_NET    1 = refuse to run until net_probe has passed here
#   CASE_SHELL        one helper that produces the whole log (a launch)
#
# A case that pushes data clears <dataDir>/inbox first (delete, then wait for the
# listing to empty): leftover fixtures from an earlier sitting would move every
# cell the scripts press on, and the picker order is what the scripts encode.
#
# Servers are the user-land ones from SPEC Appendix C: impacket's smbserver on
# port 1445 and anyvm-org's nfsd, both under $SPLAT_DATA_DIR/runs/v0.2-library
# (override with SPLAT_SMB_SERVER / SPLAT_NFS_SERVER / SPLAT_SMB_SHARE /
# SPLAT_NFS_EXPORT). Nothing here needs root, a NAS or a Docker image.
#
# Results: $SPLAT_DATA_DIR/runs/v0.2-library/console-results/<case>/
#   viewer.log, the shots, pass (a marker with the script's sha256) or fail
#   (the assertion that failed).
#
# Exit: 0 every case passed (or was already passing), 1 a case failed, 2 usage,
# 3 the console is not reachable / a server did not start.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

usage() {
    awk 'NR > 1 && /^set -uo pipefail/ { exit } NR > 1 { sub(/^# ?/, ""); print }' \
        "${BASH_SOURCE[0]}"
}

mode=run
only=""
force=0
stop_on_fail=0
replay_dir=""
# fresh_identity installs a title identity and is never part of a default run
# (AGENTS.md); --with-fresh-identity is the only way in.
with_fresh_identity=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --list) mode=list ;;
        --dry-run) mode=dry ;;
        --replay)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            mode=replay
            replay_dir="$1" ;;
        --replay=*) mode=replay; replay_dir="${1#--replay=}" ;;
        --run) mode=run ;;
        --only)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            only="${1//-/_}" ;;
        --only=*) only="${1#--only=}"; only="${only//-/_}" ;;
        --force) force=1 ;;
        --with-fresh-identity) with_fresh_identity=1 ;;
        --stop-on-fail) stop_on_fail=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "console-run: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

data="$(ps5_data_dir)"
library="$data/runs/v0.2-library"
results="${SPLAT_CONSOLE_RESULTS:-$library/console/results}"
smb_server="${SPLAT_SMB_SERVER:-$library/venv-smb/bin/python $library/venv-smb/bin/smbserver.py}"
smb_share="${SPLAT_SMB_SHARE:-$library/smb-share}"
nfs_server="${SPLAT_NFS_SERVER:-python3 $library/ref/nfsd/nfsd.py}"
nfs_export="${SPLAT_NFS_EXPORT:-$library/nfs-export}"
smb_user="${SPLAT_SMB_USER:-test}"
smb_password="${SPLAT_SMB_PASSWORD:-test}"
smb_port="${SPLAT_SMB_PORT:-1445}"
nfs_port="${SPLAT_NFS_PORT:-2049}"
lan_host="${SPLAT_LAN_HOST:-}"

note() { printf 'console-run: %s\n' "$*" >&2; }
fail_hard() { note "$*"; exit "${2:-1}"; }

# --- the cases -------------------------------------------------------------
# Each function only describes the case; run_one() does the work, and the same
# description is replayed against saved logs by --replay.

CASE_NAMES=()
CASE_TITLES=()
CASE_CURRENT_NAME=""

register() {  # register <name> <one-line title>
    CASE_NAMES+=("$1")
    CASE_TITLES+=("$2")
}

reset_case() {
    CASE_SCRIPT=""
    CASE_EXPECT=()
    CASE_REJECT=()
    CASE_SHOTS=()
    CASE_LOCATIONS=""
    CASE_SERVERS=""
    CASE_STOP_SERVER=0
    CASE_DATA=""
    CASE_INBOX_NAMES=""
    CASE_FREE_SPACE=0
    CASE_ENV=()
    CASE_SECONDS=0
    CASE_CHECK=""
    CASE_PRE=""
    CASE_SHELL=""
    CASE_NEEDS_NET=""
    CASE_LOG_DIR=""
}

case_formats() {
    CASE_SCRIPT="convert-formats.txt"
    CASE_DATA="console/photos"
    CASE_PRE="ensure_album"
    CASE_SECONDS=120
    CASE_EXPECT=(
        'viewer: jobs: jpg_orient6.jpg done'
        'viewer: jobs: png.png done'
        'viewer: jobs: webp.webp done'
        'viewer: jobs: avif.avif done'
        'viewer: jobs: tiff.tif done'
        'viewer: jobs: bmp.bmp done'
        'viewer: jobs: gif.gif done'
        'viewer: jobs: tga.tga done'
        'viewer: jobs: heic.heic done'
        'photo is 70000x1000 \(70\.0 MP\)'
        'viewer: jobs: the queue is idle again \(peak RSS [0-9.]+ [KMG]iB\)'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL')
}

# The inbox names of the splat fixtures. The good files come first, so the
# picker's first cells are the ones import-splats.txt presses Cross on and the
# malformed ones follow. The prefix also makes the imported scenes ("0a-big",
# ...) sort before every converted scene in the album, which is what makes
# imported-view.txt's first scene an import and not "avif".
splat_inbox_names() {
    case "$1" in
        big.splat) echo "0a-big.splat" ;;
        tiny.spz) echo "0b-tiny.spz" ;;
        tiny.splat) echo "0c-tiny.splat" ;;
        tiny.compressed.ply) echo "0d-tiny.compressed.ply" ;;
        tiny.ply) echo "0e-tiny.ply" ;;
        bad.splat) echo "0f-bad.splat" ;;
        bad.spz) echo "0g-bad.spz" ;;
        bad.compressed.ply) echo "0h-bad.compressed.ply" ;;
        bad.ply) echo "0i-bad.ply" ;;
        *) echo "0z-$1" ;;
    esac
}

case_imports() {
    CASE_SCRIPT="import-splats.txt"
    CASE_DATA="console/splats"
    CASE_INBOX_NAMES="splat_inbox_names"
    CASE_PRE="ensure_album"
    CASE_SECONDS=90
    # The malformed fixtures are not asserted here: the picker shows six tiles
    # per row and only the first six are reachable with Right, so which of the
    # nine fixtures the script presses depends on the upload order. The refusal
    # of all four is its own case (import_refusals) with only those four files.
    CASE_EXPECT=(
        'viewer: jobs: 0b-tiny\.spz done: imported [0-9,]+ splats'
        'viewer: jobs: 0c-tiny\.splat done'
        'viewer: jobs: 0d-tiny\.compressed\.ply done'
        'thinned 3,000,000 to 2,000,000 splats'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL')
}

case_imported_renders() {
    # A2's last step, numeric: open the imported scene and count the pixels that
    # are not the backdrop (A2 asks for > 5 %). The scene is named: the album
    # also holds the format case's converted scenes, and "avif" sorts first
    # among those - an assertion on the name is what keeps this from measuring a
    # conversion, which is what the first version did.
    CASE_SCRIPT="imported-view.txt"
    CASE_PRE="ensure_album"
    CASE_SECONDS=40
    CASE_EXPECT=('viewer: tv scene 1/[0-9]+ 0a-big \(album 00_v02_console\)'
                 'viewer: script done')
    CASE_REJECT=('^viewer: .*FAIL')
    CASE_SHOTS=("imported-scene:0.05")
}

case_import_refusals() {
    # T10: the four malformed splat files are refused, each with its own
    # reader's reason. Only these fixtures are pushed, so all four are among the
    # panel's six reachable tiles whatever order the inbox lists them in; the
    # desktop test (tests/splat_refusals_test.cpp) pins the same four messages.
    CASE_SCRIPT="import-refusals.txt"
    CASE_DATA="console/splats-bad"
    CASE_INBOX_NAMES="splat_inbox_names"
    CASE_PRE="ensure_album"
    CASE_SECONDS=60
    CASE_EXPECT=(
        'viewer: jobs: 0f-bad\.splat failed: cannot import splat: unsupported splat file'
        'viewer: jobs: 0g-bad\.spz failed: cannot import splat: gzip stream ended before the declared splat data'
        'viewer: jobs: 0h-bad\.compressed\.ply failed: cannot import splat: compressed PLY has too few chunks'
        'viewer: jobs: 0i-bad\.ply failed: cannot import splat: \.ply vertex data is truncated'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'unknown location')
}

case_net_preflight() {
    # T2: the console pass spent ~50 minutes on four working-share cases and two
    # fallback cases against a share that had never answered once. This is the
    # 20-second check that has to pass first: one location, one IP, one ready
    # line. Everything that needs a share refuses to run without it
    # (CASE_NEEDS_NET).
    CASE_SCRIPT="net-probe.txt"
    CASE_SERVERS="smb"
    CASE_LOCATIONS="smb"
    CASE_SECONDS=25
    CASE_EXPECT=(
        'viewer: location smb0 ready'
        'viewer: net: sceNetInit -> '
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'offline' 'unknown location')
    CASE_CHECK="check_net_preflight"
}

case_net_probe() {
    # "Start the Mac servers, mount an SMB and an NFS location by IP, assert the
    # ready lines and the connection in each server's own log, clean up, and
    # print PASS or FAIL per protocol." No scene, no panel navigation: the
    # locations are mounted by the viewer at start. Under three minutes.
    #
    # When runs/v0.2-library/mac-share/READY exists the SMB half uses that test
    # share (Apple's own server, port 445, the one exception to the port rule in
    # AGENTS.md: only that share, that user, no listing); otherwise it uses the
    # generated impacket share on port 1445 that every other SMB case starts.
    CASE_SCRIPT="net-probe.txt"
    # With the Mac's own share ready there is no impacket server to start for
    # SMB: the location points at Apple's server, so only nfsd comes up here.
    if mac_share_ready; then
        CASE_SERVERS="nfs"
    else
        CASE_SERVERS="smb nfs"
    fi
    CASE_LOCATIONS="probe"
    CASE_SECONDS=30
    CASE_EXPECT=(
        'viewer: location smb0 ready'
        'viewer: location nfs0 ready'
        # A digit, not a minus: the stack line has to say the stack came up.
        'viewer: net: sceNetInit -> [0-9]'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'offline' 'unknown location')
    CASE_CHECK="check_net_probe"
}

case_normal_launch() {
    # T9: a sitting must end with the title in *normal* launch mode. The pass
    # left bench/viewer.env as MODE=tv/UI=1/SCRIPT=... (written by an earlier
    # ui-test.sh), so starting the title from the home screen ran a scripted
    # pass and quit. This case writes an unscripted selector, launches once and
    # requires that no "viewer: script " line appears.
    CASE_SHELL="normal_launch_step"
    CASE_EXPECT=(
        'viewer: start \('
        'viewer: PASS, exit 0'
    )
    CASE_REJECT=('viewer: script ')
}

case_raw_photos() {
    # P3: the RAW decode path was computed and thrown away, so the release claim
    # ("the RAW thumbnail is the camera's preview, not a dark frame") could not
    # be checked from a log. The decoder's note is on the job's own done line
    # now, and the two samples take different paths: the Canon CR2 carries a
    # JPEG preview, the Sigma DNG does not and gets the half-size demosaic.
    CASE_SCRIPT="raw-photos.txt"
    CASE_DATA="console/raw"
    CASE_PRE="ensure_album"
    CASE_SECONDS=120
    CASE_EXPECT=(
        'viewer: jobs: canon-powershot-s90\.cr2 done: raw preview \(Canon\)'
        'viewer: jobs: sigma-fp-12bit\.dng done: half-size demosaic \(Sigma\)'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL')
}

case_smb_library() {
    # A3.1/A3.6: the share lists and a scene opens in place. The scene is the
    # one smb_save_to_share uploaded (this case runs after it, and says so when
    # the share is empty), and a remote scene is opened with OpenPath - the line
    # the app really writes - not the local album's "(album ...)" form.
    CASE_SCRIPT="network-library.txt"
    CASE_SERVERS="smb"
    CASE_LOCATIONS="smb"
    CASE_NEEDS_NET=1
    CASE_SECONDS=70
    CASE_EXPECT=(
        'viewer: location smb0 ready'
        'viewer: tv scene .* \(opened from smb0\)'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'unknown location')
    CASE_CHECK="check_share_scene_any"
    # T4: a band, not a floor. The console pass "passed" this shot with content
    # 1.0000 - the dark Locations screen, not a rendered scene. A real scene
    # frame is a few to ninety percent non-backdrop; a UI screen is a flat
    # colour. Both ends are asserted now.
    CASE_SHOTS=("remote-scene:0.05:0.90")
}

case_smb_save_to_share() {
    # A3.3/A3.4: convert a photo *into* the share; the four files land on the
    # Mac (checked by check_share_scene) and tmp has no staging folder left.
    # The free-space check is the second half of A3.4: a copy that was deleted
    # cannot consume space, and this measures it instead of inferring it.
    CASE_SCRIPT="save-to-share.txt"
    CASE_SERVERS="smb"
    CASE_LOCATIONS="smb"
    CASE_NEEDS_NET=1
    CASE_DATA="console/photos"
    CASE_PRE="note_share_scenes"
    CASE_FREE_SPACE=4
    CASE_SECONDS=90
    CASE_EXPECT=(
        'viewer: location smb0 ready'
        'viewer: jobs: .* done'
        'viewer: jobs: the queue is idle again'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'upload failed')
    CASE_CHECK="check_share_scene check_tmp_empty"
}

case_smb_server_stopped() {
    # A3.5: the server is gone, so a conversion into the share must fail with
    # the location named, keep the staging copy, and leave the panel alive.
    #
    # T6: the console pass "passed" this on the resolver error - the share had
    # never been reachable, so stopping the server made no difference. The case
    # now (a) requires net_probe to have passed in this sitting, so the share
    # really worked earlier in the same boot, and (b) requires a *connection*
    # failure: a refused connect, never an address that could not be resolved.
    CASE_SCRIPT="save-to-share.txt"
    CASE_SERVERS="smb"
    CASE_STOP_SERVER=1
    CASE_LOCATIONS="smb"
    CASE_NEEDS_NET=1
    CASE_DATA="console/photos"
    CASE_SECONDS=90
    CASE_EXPECT=(
        "viewer: jobs: .* failed: upload failed: .*smb0.*kept at $(ps5_title_data_dir)/tmp/"
        'viewer: jobs: .* failed: upload failed: .*smb0: cannot connect to .*(Connection refused|errno)'
        'viewer: script done'
    )
    # "unknown location" is the boot-race bug, not a stopped server: that path
    # must not count as this case passing (the console pass's 09:24 run was
    # exactly that and read as an upload failure). An unresolved address is the
    # same false pass (T6).
    CASE_REJECT=('^viewer: .*\bhang' 'watchdog.*(kill|KILL)' 'unknown location'
                 'Invalid address' 'cannot resolve' 'getaddrinfo')
}

case_import_from_share() {
    # The last "not covered" extra: a splat imported *from* the share. The
    # driver puts one .splat file in the location's folder (newest mtime, so the
    # Photos tab lists it first), the script imports it with Cross, and the
    # imported scene is uploaded back to the share (the job's output root is the
    # remote library) - check_share_import proves the new folder is there.
    CASE_SCRIPT="import-from-share.txt"
    CASE_SERVERS="smb"
    CASE_LOCATIONS="smb"
    CASE_NEEDS_NET=1
    CASE_PRE="seed_share_splat note_share_scenes"
    CASE_SECONDS=60
    CASE_EXPECT=(
        'viewer: location smb0 ready'
        'viewer: jobs: .*0-tiny\.splat done: imported [0-9,]+ splats'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'unknown location')
    CASE_CHECK="check_share_import"
}

case_nfs_library() {
    # A3.2: the mount probe must answer "ready" instead of hanging, and the
    # library must list and open a scene (the same one the SMB save uploaded,
    # copied into the export by seed_nfs_export). The end-to-end list/read/write
    # inside the export is proven on the desktop against the same user-land nfsd
    # (tests/vfs_nfs_test.cpp, [nfs-live], platform/ps5/scripts/live-servers.sh).
    CASE_SCRIPT="network-library.txt"
    CASE_SERVERS="nfs"
    CASE_LOCATIONS="nfs"
    CASE_NEEDS_NET=1
    CASE_PRE="seed_nfs_export"
    CASE_SECONDS=70
    CASE_EXPECT=(
        'viewer: location nfs0 ready'
        'viewer: tv scene .* \(opened from nfs0\)'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL' 'unknown location')
}

case_offline_fallback() {
    # A3.2's "not reached" half: with nothing listening, the location must
    # report offline within the timeout instead of hanging, and the panel must
    # still navigate (the script reaches its own end).
    #
    # T5: the console pass passed this on any offline line, including the
    # resolver error, and with no timing at all - so A3.2's "within 7 s" was not
    # proven. The state now carries the mount's own elapsed time ("offline after
    # 0.4 s: ..."), check_offline_seconds asserts it, and a resolver failure is
    # rejected: this case is about a server that does not answer, not an address
    # that cannot be looked up.
    CASE_SCRIPT="network-library.txt"
    CASE_LOCATIONS="smb-dead"
    CASE_SECONDS=60
    CASE_EXPECT=(
        'viewer: location smb0 offline after [0-9]+\.[0-9] s: .*'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*\bhang' 'watchdog.*(kill|KILL)' 'Invalid address'
                 'cannot resolve' 'getaddrinfo')
    CASE_CHECK="check_offline_seconds"
}

case_rotation_persists() {
    # Stage P2: rotation is a file action, not a viewer feature. rotate-local.txt
    # turns a JPEG in the picker (right then left: the source's Orientation tag
    # is rewritten, "file updated") and turns the first scene right then left as
    # a background job. This fresh launch reopens the album's first scene: the
    # scene must still open, and there is no viewer rotation to read back
    # (DECISIONS.md, "Change 2026-11-05").
    CASE_SCRIPT="reopen-rotation.txt"
    CASE_DATA="console/photos"   # one real photo for the picker turn
    CASE_PRE="ensure_album pre_rotate_scene"
    CASE_SECONDS=60
    CASE_EXPECT=(
        'viewer: tv scene .* (opened from|album)'
        'viewer: script done'
    )
    CASE_REJECT=('^viewer: .*FAIL')
}

# A6: the throwaway identity is installed, verified, launched once and removed
# again - the check that a first-time user gets a working title and no leftovers.
# The package has to be built for that identity first
# (make -C platform/ps5/viewer IDENTITY=fresh); without it the case says so.
# The identity, its package folder and its data folder all come from
# platform/ps5/title/identities.json (ps5_title_id/ps5_title_package): a
# hard-coded PPSA… here disagreed with IDENTITY=fresh the moment the table
# changed, and the case then failed for the wrong reason.
fresh_identity_step() {
    local title_id package
    title_id="$(ps5_title_id fresh)" || return 2
    package="$(ps5_title_package fresh)" || return 2
    if [ ! -d "$package" ] || [ ! -f "$package/eboot.bin" ]; then
        echo "fresh-identity: SKIP: no $package (make -C platform/ps5/viewer IDENTITY=fresh)"
        return 3
    fi
    local title_id_sh="$here/title.sh"
    if ! "$title_id_sh" install "$package"; then
        echo "fresh-identity: FAIL: install failed"
        return 1
    fi
    if "$title_id_sh" status "$package"; then
        echo "fresh-identity: installed and verified $title_id"
    else
        echo "fresh-identity: FAIL: status does not verify $title_id"
        return 1
    fi
    if ! "$here/title-uninstall.sh" --yes "$title_id"; then
        echo "fresh-identity: FAIL: uninstall failed"
        return 1
    fi
    if "$title_id_sh" status "$package" >/dev/null 2>&1; then
        echo "fresh-identity: FAIL: $title_id is still registered after the uninstall"
        return 1
    fi
    echo "fresh-identity: uninstalled $title_id and the identity is gone"
    return 0
}

case_fresh_identity() {
    CASE_SHELL="fresh_identity_step"
    local title_id
    title_id="$(ps5_title_id fresh)" || title_id="the fresh identity"
    CASE_EXPECT=("installed and verified $title_id" "uninstalled $title_id and the identity is gone")
}

# A crash check has no script: it scans every case's klog for a crash marker.
# Kept as a registered case so --list and --only can name it, and so a failing
# scan lands in the results like any other case.
scan_crashes() {
    local log hit=0
    # T7: the failed attempts' logs live in the run folders under
    # $SPLAT_DATA_DIR/runs/ui_v1/shots, not in $results, so the console pass's
    # crashed attempts were never scanned. Both sets are scanned now.
    for log in "$results"/*/viewer.log "$data"/runs/ui_v1/shots/*/viewer.log \
        "$results"/*/normal-launch.log; do
        [ -f "$log" ] || continue
        if grep -Eqi 'tombstone|core dump|SIGSEGV|SIGABRT|unhandled exception|assertion .*failed' \
            "$log"; then
            note "crash marker in $log: $(grep -Eoi 'tombstone|core dump|SIGSEGV|SIGABRT|unhandled exception' "$log" | head -1)"
            hit=1
        fi
    done
    return "$hit"
}

# The order is the run's order, and three dependencies are deliberate:
#   * net_probe runs FIRST: it proves the console can reach a share at all. Every
#     case that needs one refuses to run until it has passed in this sitting, so
#     a dead LAN cannot look like six product failures (T2).
#   * imported_renders must run right after imports and *before* formats, so the
#     album it opens holds the imports (a converted scene sorts before "0a-big"
#     only if its name starts lower, and the name assertion covers the rest).
#   * smb_library must run after smb_save_to_share, whose upload is the scene it
#     opens (a clean share has no scene of its own).
# normal_launch is last but one: it leaves the console in the state a user finds
# it in, and crash_check then scans everything this sitting wrote.
register net_probe "network: SMB and NFS by IP, ready in the viewer and seen by each server"
register net_preflight "network: one share answers before the rest of the pass runs"
register imports "A2: spz/splat/compressed.ply import, 3 M thinning"
register import_refusals "A2: the four malformed splat files are refused, each with its reason"
register imported_renders "A2: the imported scene really draws (>5 % non-backdrop)"
register formats "A1: nine photo formats, the 70 MP guard, the peak-RSS line"
register raw_photos "A1: two real camera RAW files, the decode path named in the log"
register smb_save_to_share "A3.3/A3.4: save to the share, four files, tmp and free space"
register smb_library "A3.1/A3.6: SMB ready, library listed, scene opened in place"
register smb_server_stopped "A3.5: server stopped, upload fails and keeps the copy"
register import_from_share "A2 extra: a splat imported from the share lands on it"
register nfs_library "A3.2: NFS ready, library listed, scene opened"
register offline_fallback "A3.2: an unreachable location goes offline within 7 s"
register rotation_persists "photo and scene file turns, then a relaunch"
register fresh_identity "A6: the throwaway identity installs, verifies and uninstalls"
register normal_launch "the title starts unscripted and reaches its own end"
register crash_check "no crash/tombstone line in any case's log"

# --- helpers ---------------------------------------------------------------

case_title() {
    local want="$1" index=0 name
    for name in "${CASE_NAMES[@]}"; do
        if [ "$name" = "$want" ]; then printf '%s' "${CASE_TITLES[$index]}"; return 0; fi
        index=$((index + 1))
    done
    printf '?'
}

selected() {  # selected <name>
    [ -z "$only" ] || case ",$only," in *",$1,"*) return 0 ;; esac
    [ -n "$only" ] && return 1
    return 0
}

# The script a case runs, resolved next to this file.
case_script_path() { printf '%s/ui-tests/%s' "$here" "$1"; }

# The locations.json a case pushes: the LAN address of this Mac with the test
# server's port, so the console reaches it. Never a personal path (AGENTS.md).
#
# Only the fields whose value differs from Location's own default are written:
# locations_from_json fills the rest (core/vfs/location.cpp). A field added to
# the schema therefore needs no change here - the old copy listed every key and
# silently dropped any new one.
write_locations() {  # write_locations <kind> <file>
    local kind="$1" file="$2" host="$lan_host"
    [ -n "$host" ] || fail_hard "set SPLAT_LAN_HOST to this Mac's LAN IP (the console has to reach it)" 3
    case "$kind" in
        smb)
            cat >"$file" <<JSON
{
  "version": 1,
  "locations": [
    {"id":"smb0","protocol":"smb","host":"$host","port":$smb_port,"share":"PHOTOS","path":"library","user":"$smb_user","password":"$smb_password","photo_source":true,"splat_library":true}
  ]
}
JSON
            ;;
        smb-dead)
            # Port 1: nothing listens there, so the probe must time out.
            cat >"$file" <<JSON
{
  "version": 1,
  "locations": [
    {"id":"smb0","protocol":"smb","host":"$host","port":1,"share":"PHOTOS","path":"","user":"$smb_user","password":"$smb_password","photo_source":true,"splat_library":true}
  ]
}
JSON
            ;;
        nfs)
            # AUTH_SYS: the export is owned by this user, so uid/gid are the
            # caller's (the live test found 65534 refused with NFS3ERR_ACCES).
            cat >"$file" <<JSON
{
  "version": 1,
  "locations": [
    {"id":"nfs0","protocol":"nfs","host":"$host","port":$nfs_port,"share":"/","path":"","photo_source":true,"splat_library":true,"uid":$(id -u),"gid":$(id -g)}
  ]
}
JSON
            ;;
        probe)
            # net_probe: one SMB share and one NFS export, side by side, by IP
            # and port - nothing else, so a failure is one protocol's.
            local probe_host="$host" probe_port="$smb_port" probe_share="PHOTOS"
            local probe_user="$smb_user" probe_password="$smb_password"
            local source="the generated impacket share on port $smb_port"
            if mac_share_ready; then
                # The user's own case: a regular macOS shared folder. Only the
                # share and the user named in creds.txt, port 445, no listing.
                probe_host="$(mac_share host)"
                probe_port="$(mac_share port)"
                probe_share="$(mac_share share)"
                probe_user="$(mac_share user)"
                probe_password="$(mac_share password)"
                source="the mac-share test share \"$probe_share\" on port $probe_port"
            fi
            note "net_probe: SMB uses $source"
            cat >"$file" <<JSON
{
  "version": 1,
  "locations": [
    {"id":"smb0","protocol":"smb","host":"$probe_host","port":$probe_port,"share":"$probe_share","path":"","user":"$probe_user","password":"$probe_password"},
    {"id":"nfs0","protocol":"nfs","host":"$host","port":$nfs_port,"share":"/","path":"","uid":$(id -u),"gid":$(id -g)}
  ]
}
JSON
            ;;
        *) fail_hard "unknown locations kind '$kind'" 2 ;;
    esac
}

# Starts the user-land test servers in the background and records their pids.
#
# T1: each server logs into *its case's* folder. One shared $results/<kind>-
# server.log was truncated by every start, so the evidence of the case that had
# just failed was gone by the time anyone read it (the console pass lost four
# server logs that way and could only infer "no connection").
SERVER_PIDS=()
start_servers() {  # start_servers "<kinds>"
    local kind log_dir
    log_dir="${CASE_LOG_DIR:-$results}"
    for kind in $1; do
        case "$kind" in
            smb)
                [ -d "$smb_share" ] || mkdir -p "$smb_share/photos"
                # An empty sub-folder as the location's Path: the library root
                # then has no scenes of its own, so its first tile is "+ Add
                # photos" and the scripts do not depend on what an earlier run
                # left there.
                mkdir -p "$smb_share/library"
                # shellcheck disable=SC2086
                $smb_server -smb2support -port "$smb_port" -username "$smb_user" \
                    -password "$smb_password" PHOTOS "$smb_share" \
                    >"$log_dir/smb-server.log" 2>&1 &
                SERVER_PIDS+=("$!")
                ;;
            nfs)
                [ -d "$nfs_export" ] || mkdir -p "$nfs_export"
                # shellcheck disable=SC2086
                $nfs_server -dir "$nfs_export" -port "$nfs_port" -vers 3 -v \
                    >"$log_dir/nfs-server.log" 2>&1 &
                SERVER_PIDS+=("$!")
                ;;
            "") return 0 ;;
            *) fail_hard "unknown server '$kind'" 2 ;;
        esac
    done
    sleep 2
    local pid
    for pid in ${SERVER_PIDS[@]+"${SERVER_PIDS[@]}"}; do
        kill -0 "$pid" 2>/dev/null ||
            fail_hard "a test server did not stay up; see $log_dir/*-server.log" 3
    done
}

stop_servers() {
    local pid
    for pid in ${SERVER_PIDS[@]+"${SERVER_PIDS[@]}"}; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    SERVER_PIDS=()
}

# True when this Mac's own test share is ready (runs/v0.2-library/mac-share/
# READY). The user's real setup is a regular macOS shared folder, so Apple's
# server is the case that matters most; net_probe uses that share when it
# exists, and only that share, that user and port 445 - the single exception to
# the port rule in AGENTS.md, and no listing.
mac_share_ready() { [ -f "$library/mac-share/READY" ] && [ -f "$library/mac-share/creds.txt" ]; }

# mac_share <key>: host, port, share, user or password out of creds.txt.
mac_share() {
    sed -n "s/^$1=//p" "$library/mac-share/creds.txt" | head -1
}

# The first half of the rotation check: a JPEG turn and a scene turn through
# the panel (ui-tests/rotate-local.txt). The second half is the case itself, a
# fresh launch that must still open the scene and finish the script.
# ---- console-side file helpers --------------------------------------------
# ProsperoMgr's file API is the only route: GET /api/ftp/list, POST
# /api/ftp/mkdir, POST /api/ftp/delete, and title.sh put for one file. Every
# path goes through ps5_guard_remote_path, and only the selected identity's data
# folder is touched (AGENTS.md).

remote_ensure_dir() {  # remote_ensure_dir <dir>
    local dir="$1"
    ps5_ftp_list_checked "$dir" 0 >/dev/null 2>&1 && return 0
    ps5_http POST /api/ftp/mkdir json "$(ps5_json_path "$dir")" >/dev/null 2>&1 ||
        fail_hard "cannot create $dir on the console" 4
}

# remote_delete_all <dir>: delete the regular files directly in <dir>, then wait
# (up to 20 s) for the listing to show none. A directory is never deleted: the
# inbox holds the fixtures this run pushed, and nothing else may be touched.
remote_delete_all() {
    local dir="$1" listing name waited=0 names
    names="$(mktemp "${TMPDIR:-/tmp}/console-run-names.XXXXXX")" || return 1
    listing="$(ps5_ftp_list_checked "$dir" 0)" || { rm -f "$names"; return 1; }
    printf '%s\n' "$listing" | awk -F'\t' '$3 == "file" { print $1 }' >"$names"
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        ps5_http POST /api/ftp/delete json "$(ps5_json_path "$dir/$name")" >/dev/null || {
            note "cannot delete $dir/$name from the console"
            rm -f "$names"
            return 1
        }
    done <"$names"
    rm -f "$names"
    while :; do
        listing="$(ps5_ftp_list_checked "$dir" 0)" || return 1
        [ -n "$listing" ] || return 0
        waited=$((waited + 1))
        [ "$waited" -ge 20 ] && return 1
        sleep 1
    done
}

# push_inbox <local dir> [name function]: upload every file in <dir> to
# <dataDir>/inbox, renamed by the case's function when one is given, so the
# picker's cell order is the case's and not the inbox's. The inbox is cleared
# first: a leftover fixture from an earlier sitting moves every cell.
push_inbox() {
    local dir="$1" namer="${2:-}" file name count=0 remote_inbox
    [ -d "$dir" ] || { note "no fixture folder $dir"; return 1; }
    remote_inbox="$(ps5_title_data_dir)/inbox"
    remote_ensure_dir "$remote_inbox"
    remote_delete_all "$remote_inbox" || {
        note "cannot empty $remote_inbox (a delete failed or the console is unreachable)"
        return 1
    }
    for file in "$dir"/*; do
        [ -f "$file" ] || continue
        name="$(basename "$file")"
        [ -n "$namer" ] && name="$("$namer" "$name")"
        "$here/title.sh" put "$file" "$remote_inbox/$name" >/dev/null ||
            { note "uploading $file to $remote_inbox/$name failed"; return 1; }
        count=$((count + 1))
    done
    note "inbox: $count fixture file(s) in $remote_inbox"
    return 0
}

# The Mac side of A3.3: the share must hold a scene folder with the four files.
share_scenes() {  # share_scenes [dir]: scene folders on the share, one per line
    local root="${1:-$smb_share/library}"
    find "$root" -mindepth 1 -maxdepth 2 -type d -exec test -f '{}/scene.psplat' \; \
        -print 2>/dev/null | sort
}

# The folders that existed before the case ran. Without this snapshot a check
# passes on the folder a previous sitting left, whatever this run did.
note_share_scenes() {
    local log_dir="$1"
    share_scenes >"$log_dir/share-before.txt"
    note "share: $(wc -l <"$log_dir/share-before.txt" | tr -d ' ') scene folder(s) before the run"
}

# new_share_scene <log_dir>: the first scene folder on the share that is not in
# the case's before-snapshot.
new_share_scene() {
    local log_dir="$1" candidate
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        if [ -f "$log_dir/share-before.txt" ] &&
            grep -qxF "$candidate" "$log_dir/share-before.txt"; then
            continue
        fi
        printf '%s\n' "$candidate"
        return 0
    done <<EOF
$(share_scenes)
EOF
    return 1
}

# check_share_scene: this run added a scene folder with the four files.
check_share_scene() {
    local log_dir="$results/$CASE_CURRENT_NAME" folder name
    folder="$(new_share_scene "$log_dir")" ||
        { note "no new scene folder under $smb_share/library"; return 1; }
    for name in original.jpg thumb.jpg scene.psplat meta.json; do
        [ -s "$folder/$name" ] || { note "$folder has no $name"; return 1; }
    done
    note "share scene: $folder ($(wc -c <"$folder/scene.psplat" | tr -d ' ') bytes)"
    return 0
}

# The precondition of smb_library: the share holds a scene to open (uploaded by
# smb_save_to_share, which runs first).
check_share_scene_any() {
    local folder
    folder="$(share_scenes | head -1)"
    [ -n "$folder" ] ||
        { note "the share has no scene to open; run smb_save_to_share first (A3.3)"; return 1; }
    note "share scene for the library: $folder"
    return 0
}

# A2 extra: the imported-from-share scene was uploaded back to the share, so a
# scene folder with meta.json's "origin" appeared that was not there before.
check_share_import() {
    local log_dir="$results/$CASE_CURRENT_NAME" candidate fresh=""
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        if [ -f "$log_dir/share-before.txt" ] &&
            grep -qxF "$candidate" "$log_dir/share-before.txt"; then
            continue
        fi
        [ -f "$candidate/meta.json" ] || continue
        if grep -q '"origin"' "$candidate/meta.json"; then fresh="$candidate"; break; fi
    done <<EOF
$(share_scenes)
EOF
    [ -n "$fresh" ] || { note "no new imported scene folder on the share"; return 1; }
    note "share import: $fresh ($(wc -c <"$fresh/scene.psplat" | tr -d ' ') bytes)"
    return 0
}

# A3.2: give the NFS export one scene to list and open - the same folder the SMB
# save uploaded, copied on this Mac (both servers serve local folders here).
seed_nfs_export() {
    local log_dir="$1" source name
    source="$(share_scenes | head -1)"
    [ -n "$source" ] ||
        { note "no scene on the share to seed the NFS export with; run smb_save_to_share first"; return 1; }
    name="$(basename "$source")"
    mkdir -p "$nfs_export"
    rm -rf "$nfs_export/$name"
    cp -R "$source" "$nfs_export/$name" ||
        { note "cannot copy $source to $nfs_export/$name"; return 1; }
    note "nfs export: seeded $nfs_export/$name"
    return 0
}

# A2 extra: one .splat file in the location's folder (the picker lists a folder
# by name, splat files last), named after the fixture so the job line is
# recognisable.
seed_share_splat() {
    local source="$library/console/splats/tiny.splat"
    [ -f "$source" ] || { note "no $source to import from the share"; return 1; }
    mkdir -p "$smb_share/library"
    cp "$source" "$smb_share/library/0-tiny.splat" ||
        { note "cannot copy $source to the share"; return 1; }
    note "share: put 0-tiny.splat in $smb_share/library"
    return 0
}

# The staging folders a finished run may legitimately leave: one per job row
# whose state is "failed" (D8 - that copy is the scene's only one). A queue file
# that cannot be read exempts nothing, which only makes check_tmp_empty stricter.
# tmp_staging_left <jobs.json> <listing file>: the names that are not exempt.
tmp_staging_left() {
    python3 - "$1" "$2" <<'PY'
import json, sys

exempt = set()
try:
    for job in json.load(open(sys.argv[1]))["jobs"]:
        if job.get("state") != "failed":
            continue
        folder = (job.get("scene_folder") or "").rstrip("/")
        if folder:
            exempt.add(folder.rsplit("/", 1)[-1])
except Exception:
    pass
left = []
for line in open(sys.argv[2]):
    parts = line.rstrip("\n").split("\t")
    if len(parts) >= 3 and parts[2] == "dir" and parts[0] not in exempt:
        left.append(parts[0])
print("\n".join(left))
PY
}

# A3.4: the console's staging folder is empty again. A failed listing is a
# failure, not an empty folder: ps5_ftp_list returned success with no output on
# any HTTP error, which is how "console unreachable" read as PASS.
check_tmp_empty() {
    local data_dir listing dirs jobs_file listing_file
    data_dir="$(ps5_title_data_dir)"
    listing="$(ps5_ftp_list_checked "$data_dir" 0)" ||
        { note "cannot list $data_dir on the console; the tmp check is unverified"; return 1; }
    if ! printf '%s\n' "$listing" | awk -F'\t' -v want="tmp" '
$1 == want && $3 == "dir" { found = 1 } END { exit found ? 0 : 1 }'; then
        note "the console has no $data_dir/tmp folder (nothing was staged)"
        return 0
    fi
    jobs_file="$(mktemp "${TMPDIR:-/tmp}/console-run-jobs.XXXXXX")" || return 1
    listing_file="$(mktemp "${TMPDIR:-/tmp}/console-run-tmp.XXXXXX")" ||
        { rm -f "$jobs_file"; return 1; }
    ps5_ftp_get_text "$data_dir/state/jobs.json" "$jobs_file" 0 2>/dev/null || : >"$jobs_file"
    if ! listing="$(ps5_ftp_list_checked "$data_dir/tmp" 0)"; then
        rm -f "$jobs_file" "$listing_file"
        note "cannot list $data_dir/tmp on the console"
        return 1
    fi
    printf '%s\n' "$listing" >"$listing_file"
    dirs="$(tmp_staging_left "$jobs_file" "$listing_file")"
    rm -f "$jobs_file" "$listing_file"
    if [ -n "$dirs" ]; then
        note "the console's tmp folder still holds: $(printf '%s' "$dirs" | tr '\n' ' ')"
        return 1
    fi
    return 0
}

# The free bytes of the console volume that holds the data folder, from
# ProsperoMgr's /api/ftp/storage (total/free per mounted volume). Non-zero when
# the console does not answer or the volume is not in the list.
remote_free_bytes() {
    local data_dir body
    data_dir="$(ps5_title_data_dir)"
    body="$(ps5_http GET /api/ftp/storage none "" 2>/dev/null)" || return 1
    printf '%s' "$body" | PS5_DATA_DIR_WANT="$data_dir" python3 -c '
import json, os, sys
want = os.environ["PS5_DATA_DIR_WANT"]
try:
    volumes = json.load(sys.stdin)["volumes"]
except Exception:
    sys.exit(1)
best = None
for volume in volumes:
    path = volume.get("path", "")
    if path and (want == path or want.startswith(path.rstrip("/") + "/")):
        if best is None or len(path) > len(best.get("path", "")):
            best = volume
if best is None:
    sys.exit(1)
print(int(best.get("free", 0)))
'
}

# The test album every case in this group converts into. list_albums() sorts the
# named albums by name and the album grid's tile 1 is the first of them, so the
# scripts' "Right then Cross" only lands on the test album when this folder
# exists. A cleaned console does not have it: without it the scripts imported
# into "Samples" (the next album) and every later assertion read the wrong album.
# The runner creates it, so a fresh console needs no manual step.
TEST_ALBUM="00_v02_console"

ensure_album() {
    local log_dir="$1" dir
    dir="$(ps5_title_data_dir)/scenes/$TEST_ALBUM"
    remote_ensure_dir "$dir" || { note "cannot create the test album $dir"; return 1; }
    note "album: $dir present"
    return 0
}

# T5: A3.2 says a share that is down goes offline "within 7 s", and only the
# mount measures that. The state reads "offline after 0.4 s: ...", so this pulls
# the number out of the log and compares it - a regex cannot.
check_offline_seconds() {
    local log="$results/$CASE_CURRENT_NAME/viewer.log" worst
    worst="$(sed -n 's/.*viewer: location [a-z0-9]* offline after \([0-9][0-9]*\.[0-9]\) s:.*/\1/p' \
        "$log" | sort -gr | head -1)"
    if [ -z "$worst" ]; then
        note "no \"offline after N s\" line in $log; the timing is unproven"
        return 1
    fi
    note "offline after ${worst} s (limit 7 s)"
    python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) <= 7.0 else 1)' "$worst" || {
        note "the location took ${worst} s to report offline (limit 7 s)"
        return 1
    }
    return 0
}

# T2: net_preflight passed only when the viewer, the mount and the share all
# agree: the ready line in the log and a connection in the server's own log.
check_net_preflight() {
    local log_dir="$results/$CASE_CURRENT_NAME"
    if [ ! -f "$log_dir/smb-server.log" ]; then
        note "no server log in $log_dir; the share's side of the mount is unproven"
        return 1
    fi
    if ! grep -Eq 'Connecting Share|connect to service' "$log_dir/smb-server.log"; then
        note "the SMB server never logged a share connect; see $log_dir/smb-server.log"
        return 1
    fi
    return 0
}

# net_probe: PASS or FAIL per protocol, with the viewer's line and the server's
# own line behind each verdict. The SMB half checks the server log only when the
# harness started that server (an impacket or Samba log in the case folder); the
# Mac's own File Sharing writes to the system log, which is not ours to read, so
# that half is proven by the viewer's ready line alone and says so.
check_net_probe() {
    local log_dir="$results/$CASE_CURRENT_NAME" log="$log_dir/viewer.log" failed=0
    if grep -Eq 'viewer: location smb0 ready' "$log"; then
        if [ -f "$log_dir/smb-server.log" ]; then
            if grep -Eq 'Connecting Share|connect to service|authenticated|AUTHENTICATE_MESSAGE' \
                "$log_dir/smb-server.log"; then
                printf 'net_probe: SMB PASS (smb0 ready; server logged the connect)\n'
            else
                printf 'net_probe: SMB FAIL (smb0 ready, but no connect in %s)\n' \
                    "$log_dir/smb-server.log" >&2
                failed=1
            fi
        else
            printf 'net_probe: SMB PASS (smb0 ready; the Mac share logs to the system log)\n'
        fi
    else
        printf 'net_probe: SMB FAIL (no "viewer: location smb0 ready" in %s)\n' "$log" >&2
        failed=1
    fi
    if grep -Eq 'viewer: location nfs0 ready' "$log"; then
        # The startup banner ("mount with: mount -t nfs ...") mentions MOUNT on
        # every run, so matching it proved nothing. "nfsd: client connected" is
        # written only when a client really opens a connection, and a missing
        # log (the harness always starts nfsd for this case) is a failure too.
        if [ ! -f "$log_dir/nfs-server.log" ]; then
            printf 'net_probe: NFS FAIL (nfs0 ready, but there is no %s)\n' \
                "$log_dir/nfs-server.log" >&2
            failed=1
        elif ! grep -Eq 'nfsd: client connected' "$log_dir/nfs-server.log"; then
            printf 'net_probe: NFS FAIL (nfs0 ready, but no client connection in %s)\n' \
                "$log_dir/nfs-server.log" >&2
            failed=1
        else
            printf 'net_probe: NFS PASS (nfs0 ready; server logged the client)\n'
        fi
    else
        printf 'net_probe: NFS FAIL (no "viewer: location nfs0 ready" in %s)\n' "$log" >&2
        failed=1
    fi
    return "$failed"
}

# T9: launch the title the way a user does - no script - and leave that selector
# in place. The pass ended with bench/viewer.env still holding SCRIPT=..., so the
# next home-screen launch ran a 43-second script and quit.
normal_launch_step() {
    local log_dir="$1" data_root env_file out rc=0
    data_root="$(ps5_title_data_dir)"
    env_file="$(mktemp "${TMPDIR:-/tmp}/console-run-env.XXXXXX")" || return 1
    printf '# written by console-run.sh: the normal, unscripted launch (T9)\nMODE=tv\nUI=1\n' \
        >"$env_file"
    remote_ensure_dir "$data_root/bench"
    "$here/title.sh" put "$env_file" "$data_root/bench/viewer.env" || { rm -f "$env_file"; return 1; }
    rm -f "$env_file"
    out="$log_dir/normal-launch.log"
    PS5_RUN_TAG=normal-launch PS5_RUN_HOST="${PS5_HOST:-}" \
    PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" PS5_RUN_SECONDS=20 PS5_RUN_OUT="$out" \
    PS5_RUN_TITLE_SH="$here/title.sh" PS5_RUN_PACKAGE="$(ps5_title_package)" \
    PS5_RUN_KLOG="$here/klog.sh" PS5_RUN_END='viewer: PASS, exit 0' \
        python3 "$here/title-capture.py" || rc=$?
    # No script to end it: the capture window closes and the title is asked to
    # stop through its own quit file, never killed.
    "$here/title.sh" stop "$(ps5_title_package)" >/dev/null 2>&1 || true
    cat "$out" 2>/dev/null
    note "normal launch: capture exited $rc; the assertions decide"
    return 0
}

# Stage P2: the prepare run turns a JPEG in the picker (right then left; the
# source's Orientation tag is rewritten, so the toast says "file updated") and
# turns the first scene right then left as a background job (the tile says
# "Rotating" and the job ends with "Rotated <name>"). Those toasts are the
# assertions: no picture and no yaw number is read, because the viewer has no
# rotation to persist (DECISIONS.md, "Change 2026-11-05").
pre_rotate_scene() {
    local log_dir="$1"
    "$here/ui-test.sh" "$here/ui-tests/rotate-local.txt" --seconds 34 \
        --pass-regex 'ui: toast Rotated' >"$log_dir/prepare.out" 2>&1 || {
        note "the rotation prepare step failed; see $log_dir/prepare.out"
        return 1
    }
    if ! grep -Eq 'ui: toast Rotated .* \(file updated\)' "$log_dir/prepare.out"; then
        note "the picker turn never logged a rewritten file (\"file updated\")"
        return 1
    fi
    if ! grep -Eq 'ui: toast Rotating .*' "$log_dir/prepare.out"; then
        note "the scene turn never queued (no \"Rotating ...\" toast)"
        return 1
    fi
    if ! grep -Eq 'ui: toast Rotated 0a-big' "$log_dir/prepare.out"; then
        note "the scene turn never finished (no \"Rotated 0a-big\" toast)"
        return 1
    fi
    return 0
}

# The shot check: the backdrop is 0.18 linear = 118 in 8 bits (SplatRenderer's
# default), so "the scene drew" is "most pixels differ from it". A flat grey
# frame is the empty pipeline, not a dark scene - the console pass's reopen shot
# was exactly that (118,118,118 over 1920x1080).
check_shot() {  # check_shot <png> min:<fraction> [max:<fraction>]
    python3 - "$1" "$2" <<'PY'
import struct, sys, zlib

def read_png(path):
    raw = open(path, 'rb').read()
    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG')
    pos, idat, width, height, depth, ctype = 8, b'', 0, 0, 0, 0
    while pos < len(raw):
        (length,) = struct.unpack('>I', raw[pos:pos + 4])
        tag = raw[pos + 4:pos + 8]
        body = raw[pos + 8:pos + 8 + length]
        if tag == b'IHDR':
            width, height, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif tag == b'IDAT':
            idat += body
        elif tag == b'IEND':
            break
        pos += 12 + length
    if depth != 8 or ctype not in (2, 6):
        raise SystemExit('unsupported PNG (depth %d type %d)' % (depth, ctype))
    channels = 3 if ctype == 2 else 4
    stride = width * channels
    data = zlib.decompress(idat)
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        kind = data[p]
        p += 1
        line = bytearray(data[p:p + stride])
        p += stride
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        out += line
        prev = line
    return width, height, channels, bytes(out)

path = sys.argv[1]
want = 0.0
ceiling = 1.0
for term in sys.argv[2:]:
    if term.startswith("min:"):
        want = float(term[4:])
    elif term.startswith("max:"):
        ceiling = float(term[4:])
w, h, ch, px = read_png(path)
total = w * h
diff = 0
sums = [0, 0, 0]
hist = {}
for i in range(0, len(px), ch):
    r, g, b = px[i], px[i + 1], px[i + 2]
    sums[0] += r
    sums[1] += g
    sums[2] += b
    if abs(r - 118) + abs(g - 118) + abs(b - 118) > 12:
        diff += 1
    hist[(r >> 3, g >> 3, b >> 3)] = 1
frac = diff / total if total else 0.0
print('shot %s %dx%d content %.4f colours %d mean %d,%d,%d' % (
    path.rsplit('/', 1)[-1], w, h, frac, len(hist),
    sums[0] // total, sums[1] // total, sums[2] // total))
sys.exit(0 if want <= frac <= ceiling else 1)
PY
}

log_has() { grep -Eq -- "$1" "$2" 2>/dev/null; }

# T2: net_probe must have passed in *this* sitting. The results folder outlives
# a sitting, so a pass recorded yesterday (or on another LAN) used to let every
# CASE_NEEDS_NET case run while net_probe itself was skipped as "already
# passed". The sitting id is written once per --run invocation; net_probe's own
# case records it beside its pass marker, and only a match counts.
net_probe_current() {
    [ -f "$results/net_probe/pass" ] || return 1
    [ -f "$results/sitting" ] || return 1
    [ -f "$results/net_probe/sitting" ] || return 1
    [ "$(head -1 "$results/sitting")" = "$(head -1 "$results/net_probe/sitting")" ]
}

# The resume stamp: everything that shapes a case's result, not only its script.
# The old stamp hashed the script plus the seconds/locations/servers/data fields
# and left the expectations out, so editing an assertion (adding a check,
# correcting a log line) kept the old `pass` marker and the case was skipped as
# "already passed" with the old result.
case_stamp() {
    local script_hash="none" text
    if [ -n "$CASE_SCRIPT" ]; then
        script_hash="$(shasum -a 256 "$(case_script_path "$CASE_SCRIPT")" | cut -d' ' -f1)"
    fi
    {
        printf 'script %s\n' "$script_hash"
        printf 'seconds %s\n' "$CASE_SECONDS"
        printf 'locations %s\n' "$CASE_LOCATIONS"
        printf 'servers %s stop %s\n' "$CASE_SERVERS" "$CASE_STOP_SERVER"
        printf 'data %s names %s\n' "$CASE_DATA" "$CASE_INBOX_NAMES"
        printf 'free %s check %s pre %s shell %s\n' "$CASE_FREE_SPACE" "$CASE_CHECK" \
            "$CASE_PRE" "$CASE_SHELL"
        printf 'env %s\n' "${CASE_ENV[*]:-}"
        printf 'expect %s\n' "${CASE_EXPECT[*]:-}"
        printf 'reject %s\n' "${CASE_REJECT[*]:-}"
        printf 'shots %s\n' "${CASE_SHOTS[*]:-}"
    } | shasum -a 256 | cut -d' ' -f1
}

# --- the runner ------------------------------------------------------------

run_one() {  # run_one <case>
    local name="$1" title log_dir log rc=0
    title="$(case_title "$name")"
    reset_case
    "case_$name"
    log_dir="$results/$name"
    log="$log_dir/viewer.log"
    CASE_CURRENT_NAME="$name"
    if [ "$mode" = dry ]; then
        note "$name: $title"
        note "  script: ${CASE_SCRIPT:-(none)}  seconds: $CASE_SECONDS"
        [ -n "$CASE_LOCATIONS" ] && note "  locations: $CASE_LOCATIONS"
        [ -n "$CASE_SERVERS" ] && note "  servers: $CASE_SERVERS$([ "$CASE_STOP_SERVER" = 1 ] && echo ' (stopped before the run)')"
        [ -n "$CASE_DATA" ] && note "  push: $CASE_DATA${CASE_INBOX_NAMES:+ (named by $CASE_INBOX_NAMES)}"
        [ "$CASE_FREE_SPACE" != 0 ] && note "  free space: may lose $CASE_FREE_SPACE MiB"
        [ -n "$CASE_PRE" ] && note "  prepare: $CASE_PRE"
        [ -n "$CASE_CHECK" ] && note "  checks: $CASE_CHECK"
        local pattern
        for pattern in "${CASE_EXPECT[@]+"${CASE_EXPECT[@]}"}"; do note "  expect: $pattern"; done
        for pattern in "${CASE_REJECT[@]+"${CASE_REJECT[@]}"}"; do note "  reject: $pattern"; done
        for pattern in "${CASE_SHOTS[@]+"${CASE_SHOTS[@]}"}"; do note "  shot:   $pattern"; done
        return 0
    fi

    mkdir -p "$log_dir"
    CASE_LOG_DIR="$log_dir"
    # fresh_identity is never part of a default run: it installs and removes a
    # title identity, and no agent may do that implicitly (AGENTS.md). Without
    # --with-fresh-identity it is reported as SKIP, not as a pass.
    if [ "$name" = fresh_identity ] && [ "$with_fresh_identity" != 1 ]; then
        note "$name: skipped (needs --with-fresh-identity: it installs and uninstalls an identity)"
        printf 'SKIP %s (needs --with-fresh-identity)\n' "$name"
        return 0
    fi
    # T2: a case that needs a share refuses to run before net_probe has passed in
    # this sitting. One 20-second check replaces an hour of ambiguous failures.
    if [ -n "$CASE_NEEDS_NET" ] && [ "$mode" = run ] && ! net_probe_current; then
        printf 'FAIL %s: the console cannot reach the share (net_probe has not passed); run --only net-probe first\n' \
            "$name" | tee -a "$log_dir/fail" >&2
        [ "$stop_on_fail" = 1 ] && exit 1
        return 1
    fi
    if [ "$mode" = run ]; then
        local marker="$log_dir/pass" stamp
        # The stamp covers everything that shapes the run, not only the script:
        # an edited expectation (a new check, a corrected log line) must re-run
        # the case instead of leaving the old pass in place.
        stamp="$(case_stamp)"
        if [ "$force" = 0 ] && [ -f "$marker" ] && [ "$(head -1 "$marker")" = "$stamp" ] &&
            { [ "$name" != net_probe ] || net_probe_current; }; then
            note "$name: already passed (skip; --force to rerun)"
            printf 'SKIP %s\n' "$name"
            return 0
        fi
        # T8: a retry must not erase the record of the attempt that failed. The
        # old fail file was removed before every attempt, so "passed on retry"
        # was unauditable.
        if [ -f "$log_dir/fail" ]; then
            mv -f "$log_dir/fail" "$log_dir/fail.1"
        fi
        rm -f "$marker"
        # T3: the console's shots folder is never cleaned by the title, so every
        # case used to pull every earlier case's PNGs into its own folder. It
        # holds files only (never a directory), and this is the title's own
        # folder, so nothing else can be touched.
        if [ -n "$CASE_SCRIPT" ]; then
            remote_delete_all "$(ps5_title_data_dir)/shots" >/dev/null 2>&1 ||
                note "$name: could not clear $(ps5_title_data_dir)/shots (earlier shots may be pulled too)"
        fi

        if [ -n "$CASE_LOCATIONS" ]; then
            write_locations "$CASE_LOCATIONS" "$log_dir/locations.json"
            local remote_locations
            remote_locations="$(ps5_title_data_dir)/state/locations.json"
            remote_ensure_dir "$(dirname -- "$remote_locations")"
            "$here/title.sh" put "$log_dir/locations.json" "$remote_locations" ||
                fail_hard "pushing locations.json failed" 4
        fi
        if [ -n "$CASE_SERVERS" ]; then
            start_servers "$CASE_SERVERS"
            if [ "$CASE_STOP_SERVER" = 1 ]; then
                note "$name: stopping the $CASE_SERVERS server before the run (A3.5)"
                stop_servers
            fi
        fi
        if [ -n "$CASE_DATA" ]; then
            # The inbox must hold exactly this run's fixtures, in this run's
            # order: a push that did not happen used to be a swallowed warning,
            # and the script then navigated an inbox that was never filled.
            if ! push_inbox "$data/runs/v0.2-library/$CASE_DATA" "$CASE_INBOX_NAMES"; then
                printf 'FAIL %s: pushing the fixtures to the console inbox failed\n' "$name" |
                    tee -a "$log_dir/fail" >&2
                stop_servers
                [ "$stop_on_fail" = 1 ] && exit 1
                return 1
            fi
        fi
        local free_before=""
        if [ "$CASE_FREE_SPACE" != 0 ]; then
            free_before="$(remote_free_bytes)" || free_before=""
            if [ -z "$free_before" ]; then
                printf 'FAIL %s: cannot read the console free space (/api/ftp/storage)\n' "$name" |
                    tee -a "$log_dir/fail" >&2
                stop_servers
                [ "$stop_on_fail" = 1 ] && exit 1
                return 1
            fi
            note "$name: free space before the run: $((free_before / 1024 / 1024)) MiB"
        fi
        if [ -n "$CASE_PRE" ]; then
            note "$name: preparing ($CASE_PRE)"
            local step ok_pre=1 step_rc step_log
            for step in $CASE_PRE; do
                # Not a subshell: a preparation step may append assertions to
                # CASE_EXPECT, which the checks below read. Its output goes to
                # the case folder as well: a prepare failure used to leave only
                # a line in the sitting's console output (T1).
                step_log="$log_dir/prepare-$step.log"
                step_rc=0
                "$step" "$log_dir" >>"$step_log" 2>&1 || step_rc=$?
                sed 's/^/  /' "$step_log" >&2
                [ "$step_rc" = 0 ] || { ok_pre=0; break; }
            done
            if [ "$ok_pre" = 0 ]; then
                printf 'FAIL %s: the preparation step %s failed (see %s/prepare.out)\n' \
                    "$name" "$CASE_PRE" "$log_dir" | tee -a "$log_dir/fail" >&2
                stop_servers
                [ "$stop_on_fail" = 1 ] && exit 1
                return 1
            fi
        fi

        local args=(--pass-regex 'viewer: script done')
        [ -n "$CASE_SCRIPT" ] || args=()
        local extra
        for extra in "${CASE_ENV[@]+"${CASE_ENV[@]}"}"; do
            args+=(--env-extra "$extra")
        done
        [ "$CASE_SECONDS" -gt 0 ] && args+=(--seconds "$CASE_SECONDS")

        if [ -n "$CASE_SHELL" ]; then
            note "$name: running the shell step $CASE_SHELL"
            # A subshell: a helper that exits on a usage error (ps5_title_id)
            # must fail this case, not end the whole sitting.
            ( "$CASE_SHELL" ) >"$log" 2>&1
            rc=$?
            sed 's/^/  /' "$log" >&2
        elif [ -z "$CASE_SCRIPT" ]; then
            note "$name: no script; only the crash check applies"
        else
            note "$name: running $(case_script_path "$CASE_SCRIPT")"
            "$here/ui-test.sh" "$(case_script_path "$CASE_SCRIPT")" "${args[@]+"${args[@]}"}" \
                >"$log_dir/ui-test.out" 2>&1 || rc=$?
            # ui-test.sh writes the klog beside its shots; copy it here.
            local newest
            newest="$(ls -dt "$data"/runs/ui_v1/shots/*-* 2>/dev/null | head -1)"
            if [ -n "$newest" ] && [ -f "$newest/viewer.log" ]; then
                cp "$newest/viewer.log" "$log"
                local png
                for png in "$newest"/*.png; do
                    [ -f "$png" ] && cp "$png" "$log_dir/" 2>/dev/null
                done
                printf '%s\n' "$newest" >"$log_dir/run-dir"
            fi
            if [ "$rc" -ne 0 ]; then
                note "$name: ui-test.sh exited $rc (see $log_dir/ui-test.out)"
                printf 'FAIL %s: ui-test.sh exited %s\n' "$name" "$rc" | tee -a "$log_dir/fail" >&2
                stop_servers
                [ "$stop_on_fail" = 1 ] && exit 1
                return 1
            fi
        fi
        stop_servers
        printf '%s\n' "$stamp" >"$marker.pending"
    fi

    # ---- the assertions (also what --replay runs) -------------------------
    [ -f "$log" ] || { note "$name: no viewer.log at $log"; return 1; }
    local pattern ok=1
    for pattern in "${CASE_EXPECT[@]+"${CASE_EXPECT[@]}"}"; do
        if ! log_has "$pattern" "$log"; then
            printf 'FAIL %s: the log has no /%s/\n' "$name" "$pattern" | tee -a "$log_dir/fail" >&2
            ok=0
        fi
    done
    for pattern in "${CASE_REJECT[@]+"${CASE_REJECT[@]}"}"; do
        if log_has "$pattern" "$log"; then
            printf 'FAIL %s: the log matches the rejected /%s/\n' "$name" "$pattern" |
                tee -a "$log_dir/fail" >&2
            ok=0
        fi
    done
    local spec shot min max rest
    for spec in "${CASE_SHOTS[@]+"${CASE_SHOTS[@]}"}"; do
        shot="${spec%%:*}"
        rest="${spec#*:}"
        min="${rest%%:*}"
        max=""
        [ "$rest" = "$min" ] || max="${rest#*:}"
        if [ ! -f "$log_dir/$shot.png" ]; then
            printf 'FAIL %s: no shot %s.png\n' "$name" "$shot" | tee -a "$log_dir/fail" >&2
            ok=0
            continue
        fi
        if ! check_shot "$log_dir/$shot.png" "min:$min" "max:$max" 2>>"$log_dir/fail"; then
            printf 'FAIL %s: %s.png is outside the %s..%s non-backdrop band\n' \
                "$name" "$shot" "$min" "${max:-1}" | tee -a "$log_dir/fail" >&2
            ok=0
        fi
    done
    if [ -n "${CASE_CHECK:-}" ] && [ "$mode" = run ]; then
        local check
        for check in $CASE_CHECK; do
            if ! "$check"; then
                printf 'FAIL %s: %s failed\n' "$name" "$check" | tee -a "$log_dir/fail" >&2
                ok=0
            fi
        done
    fi
    # A3.4's second half: a staging copy that was deleted cannot consume space,
    # so the free space must be back where it started (a few MiB of slack for
    # the queue file and the logs). A read failure is a failure, never a pass.
    if [ "${CASE_FREE_SPACE:-0}" != 0 ] && [ "$mode" = run ]; then
        local free_after
        free_after="$(remote_free_bytes)" || free_after=""
        if [ -z "$free_after" ]; then
            printf 'FAIL %s: cannot read the console free space after the run\n' "$name" |
                tee -a "$log_dir/fail" >&2
            ok=0
        else
            local lost=$((free_before - free_after))
            note "$name: free space after the run: $((free_after / 1024 / 1024)) MiB (lost $((lost / 1024 / 1024)) MiB)"
            if [ "$lost" -gt $((CASE_FREE_SPACE * 1024 * 1024)) ]; then
                printf 'FAIL %s: the console lost %s MiB of free space (more than %s MiB); the scene was not deleted locally\n' \
                    "$name" "$((lost / 1024 / 1024))" "$CASE_FREE_SPACE" |
                    tee -a "$log_dir/fail" >&2
                ok=0
            fi
        fi
    fi

    if [ "$ok" = 1 ]; then
        if [ "$mode" = run ]; then
            mv "$log_dir/pass.pending" "$log_dir/pass"
            # The sitting this pass belongs to (T2): a later invocation writes
            # a new $results/sitting, so this pass no longer satisfies the gate.
            if [ "$name" = net_probe ]; then printf '%s\n' "$sitting" >"$log_dir/sitting"; fi
        fi
        printf 'PASS %s: %s\n' "$name" "$title"
        return 0
    fi
    rm -f "$log_dir/pass.pending"
    [ "$stop_on_fail" = 1 ] && exit 1
    return 1
}

# --- main ------------------------------------------------------------------

case "$mode" in
    list)
        index=0
        for name in "${CASE_NAMES[@]}"; do
            printf '%-20s %s\n' "$name" "${CASE_TITLES[$index]}"
            index=$((index + 1))
        done
        exit 0 ;;
esac

if [ "$mode" = replay ]; then
    [ -d "$replay_dir" ] || fail_hard "--replay needs a directory of <case>/viewer.log" 2
    results="$replay_dir"
    note "replaying the assertions against $replay_dir (no console, no server)"
fi

if [ "$mode" = run ]; then
    [ -n "${PS5_HOST:-}" ] || fail_hard "set PS5_HOST or add PS5_HOST=... to .env.local" 3
    ps5_env_load
    mkdir -p "$results" || fail_hard "cannot create $results"
    [ -n "$lan_host" ] || fail_hard "set SPLAT_LAN_HOST to this Mac's LAN IP" 3
    # One id per invocation: T2's "in this sitting". The results folder persists
    # across sittings (a second sitting continues where the first stopped), so
    # the stamp is what tells a fresh reachability pass from an old one.
    sitting="$(date +%Y%m%dT%H%M%S)-$$"
    printf '%s\n' "$sitting" >"$results/sitting" ||
        fail_hard "cannot write $results/sitting" 3
fi

failed=0
passed=0
skipped=0
for name in "${CASE_NAMES[@]}"; do
    selected "$name" || continue
    if [ "$name" = crash_check ] && [ "$mode" != dry ]; then
        if [ "$mode" = replay ]; then
            out="SKIP crash_check (nothing to scan in a replay)"
            rc=0
        elif scan_crashes; then
            out="PASS crash_check: no crash marker in any case's log"; rc=0
        else
            out="FAIL crash_check: a crash marker is in the logs above"; rc=1
        fi
        printf '%s\n' "$out"
        [ "$rc" -eq 0 ] && passed=$((passed + 1)) || failed=$((failed + 1))
        continue
    fi
    out="$(run_one "$name" 2>&1)"; rc=$?
    printf '%s\n' "$out"
    case "$out" in
        *"already passed"*|*"SKIP "*) skipped=$((skipped + 1)) ;;
    esac
    if [ "$rc" -eq 0 ]; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
done

note "summary: $passed passed, $failed failed, $skipped skipped; logs in $results"
[ "$failed" -eq 0 ]
