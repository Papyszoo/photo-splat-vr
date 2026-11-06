#!/usr/bin/env bash
# The live network checks, without a NAS and without root: two user-land servers
# on this machine, the real adapters against them, one verdict per server.
#
#   platform/desktop/live-servers.sh [--build DIR] [--only smb|nfs]
#
# Why: the console pass could not show a single connection from the console to
# the Mac, so "SMB on a port other than 445", "a 40 MB scene.psplat read whole"
# and "an NFS export lists/reads/writes" had no evidence at all. They do not
# need the console - libsmb2 and libnfs run on the desktop too:
#
#   * SMB: impacket's smbserver (pure Python, no admin) on 127.0.0.1:1445. Port
#     445 needs root, so 1445 also proves the non-default-port path.
#   * NFS: anyvm-org/nfsd (one stdlib-only file, no rpcbind when nfsport and
#     mountport are given) on 127.0.0.1:2049.
#
# The cases are the [smb-live] / [nfs-live] ones in tests/vfs_smb_test.cpp and
# tests/vfs_nfs_test.cpp; they skip in the normal ctest run (no URL is set), so
# the gate stays offline and this script is where they run.
#
# Requirements (all under the data dir by default, override with the variables):
#   SPLAT_SMB_SERVER   "<python> <smbserver.py>"  default
#                      $SPLAT_DATA_DIR/runs/v0.2-library/venv-smb/bin/python
#                      + .../venv-smb/bin/smbserver.py   (pip install impacket)
#   SPLAT_NFS_SERVER   "<python> <nfsd.py>"       default
#                      python3 $SPLAT_DATA_DIR/runs/v0.2-library/ref/nfsd/nfsd.py
#   SPLAT_BUILD_DIR    the build tree with tests/splat_tests (default ./build)
#
# Exit: 0 both servers' cases passed, 1 one failed, 2 usage/missing server.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../.." && pwd)"
data="${SPLAT_DATA_DIR:-$(cd -- "$repo/.." && pwd)}"
library="$data/runs/v0.2-library"

build="${SPLAT_BUILD_DIR:-$repo/build}"
only=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build) shift; build="$1" ;;
        --build=*) build="${1#--build=}" ;;
        --only) shift; only="$1" ;;
        --only=*) only="${1#--only=}" ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "live-servers: unknown option '$1'" >&2; exit 2 ;;
    esac
    shift
done

tests="$build/tests/splat_tests"
[ -x "$tests" ] || { echo "live-servers: no $tests (build first)" >&2; exit 2; }

note() { printf 'live-servers: %s\n' "$*" >&2; }
fail=0
tmp="$(mktemp -d "${TMPDIR:-/tmp}/splat-live.XXXXXX")" || exit 2
trap 'rm -rf "$tmp"' EXIT

want() { [ -z "$only" ] || [ "$only" = "$1" ]; }

# --- SMB -------------------------------------------------------------------
smb_url="smb://127.0.0.1:1445/PHOTOS?user=test&password=test"
if want smb; then
    smb_server="${SPLAT_SMB_SERVER:-$library/venv-smb/bin/python $library/venv-smb/bin/smbserver.py}"
    if ! command -v "$(printf '%s' "$smb_server" | awk '{print $1}')" >/dev/null 2>&1; then
        note "no SMB server command ($smb_server); set SPLAT_SMB_SERVER"
        fail=1
    else
        mkdir -p "$tmp/share/library"
        # shellcheck disable=SC2086
        $smb_server -smb2support -port 1445 -username test -password test PHOTOS "$tmp/share" \
            >"$tmp/smb.log" 2>&1 &
        smb_pid=$!
        sleep 2
        if ! kill -0 "$smb_pid" 2>/dev/null; then
            note "the SMB server did not start; see $tmp/smb.log"
            fail=1
        else
            note "SMB server up on 1445 ($tmp/share)"
            SPLAT_SMB_URL="$smb_url" "$tests" -tc='*[smb-live]*' >"$tmp/smb-tests.log" 2>&1
            smb_rc=$?
            tail -n 12 "$tmp/smb-tests.log" | sed 's/^/    /' >&2
            if [ "$smb_rc" -eq 0 ]; then
                note "SMB: PASS (list, write, read, stat, unlink, 40 MB read, 4-file scene, splat in place)"
            else
                note "SMB: FAIL (exit $smb_rc); log kept at $tmp/smb-tests.log"
                cp "$tmp/smb-tests.log" "$library/smb-live-failure.log" 2>/dev/null || true
                fail=1
            fi
            kill "$smb_pid" 2>/dev/null
            wait "$smb_pid" 2>/dev/null
        fi
    fi
fi

# --- NFS -------------------------------------------------------------------
# AUTH_SYS carries a uid/gid and the server applies them to its own file system,
# so the export has to be written as the account this script runs as: 65534
# (nobody) is refused with NFS3ERR_ACCES on a folder owned by the user. That is
# the same rule a NAS export follows (all_squash/root_squash), and it is why the
# location form shows the uid and gid.
nfs_uid="$(id -u)"
nfs_gid="$(id -g)"
nfs_url="nfs://127.0.0.1/?version=3&nfsport=2049&mountport=2049&uid=$nfs_uid&gid=$nfs_gid"
if want nfs; then
    nfs_server="${SPLAT_NFS_SERVER:-python3 $library/ref/nfsd/nfsd.py}"
    nfsd_py="$(printf '%s' "$nfs_server" | awk '{print $NF}')"
    if [ ! -f "$nfsd_py" ]; then
        note "no NFS server file ($nfsd_py); set SPLAT_NFS_SERVER"
        fail=1
    else
        mkdir -p "$tmp/export/scenes"
        printf 'live nfs\n' >"$tmp/export/photo.png"
        # shellcheck disable=SC2086
        $nfs_server -dir "$tmp/export" -port 2049 -vers 3 -v >"$tmp/nfs.log" 2>&1 &
        nfs_pid=$!
        sleep 2
        if ! kill -0 "$nfs_pid" 2>/dev/null; then
            note "the NFS server did not start; see $tmp/nfs.log"
            fail=1
        else
            note "NFS server up on 2049 ($tmp/export)"
            SPLAT_NFS_URL="$nfs_url" "$tests" -tc='*[nfs-live]*' >"$tmp/nfs-tests.log" 2>&1
            nfs_rc=$?
            tail -n 12 "$tmp/nfs-tests.log" | sed 's/^/    /' >&2
            if [ "$nfs_rc" -eq 0 ]; then
                note "NFS: PASS (list, write, read, stat, unlink over NFSv3 AUTH_SYS)"
            else
                note "NFS: FAIL (exit $nfs_rc); log kept at $tmp/nfs-tests.log"
                cp "$tmp/nfs-tests.log" "$library/nfs-live-failure.log" 2>/dev/null || true
                fail=1
            fi
            kill "$nfs_pid" 2>/dev/null
            wait "$nfs_pid" 2>/dev/null
        fi
    fi
fi

[ "$fail" -eq 0 ] && note "all live server cases passed"
exit "$fail"
