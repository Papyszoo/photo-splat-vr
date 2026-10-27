#!/usr/bin/env bash
# The headset display path: install (if needed), launch and watch the vr_hello
# title, which renders a RADV test pattern into the PSVR2 eye buffers for 30 s.
#
#   platform/ps5/scripts/run-vr-hello.sh            # 30 s run, 120 s window
#   platform/ps5/scripts/run-vr-hello.sh --dry-run  # print every step
#   platform/ps5/scripts/run-vr-hello.sh --window 90
#   platform/ps5/scripts/run-vr-hello.sh --package build/ps5/title/PPSA99202
#
# The title (platform/ps5/vr_hello, id from platform/ps5/title/identities.json, the
# one app: PPSA99202) takes no input: 30 s and the pattern are compiled in, and
# a title launched through ProsperoMgr gets no argv. It installs the package
# when `title.sh status` does not verify it, connects the klog stream, launches,
# and captures until `vrhello: done, exit` or `vrhello: FAIL` appears (or the
# window closes), then `title.sh stop` (a no-op after a normal end; it never
# kills).
#
# Because every test is that same title, installing this eboot replaces the
# viewer's in place: the run says so, and run-viewer.sh installs the viewer
# again (its own `title.sh status` sees the size mismatch).
#
# Exit: 0 the end line appeared, 1 it did not within the window, 2 usage or a
# missing package, 3 no log stream, 4 install/launch failed, 5 the capture
# stayed empty or the title did not stop.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"
ps5_env_load

usage() {
    sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

dry=0
window=""
package=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --window)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            window="$1" ;;
        --window=*) window="${1#--window=}" ;;
        --package)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            package="$1" ;;
        --package=*) package="${1#--package=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "run-vr-hello: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ -n "$window" ]; then
    case "$window" in
        *[!0-9]*) echo "run-vr-hello: --window must be a whole number, got '$window'" >&2; exit 2 ;;
    esac
else
    # 30 s of pattern plus Hmd2 setup (the vblank gate alone allows 5 s) and
    # teardown.
    window=120
fi

note() { printf 'run-vr-hello: %s\n' "$*" >&2; }

title_id="$(ps5_title_id)"
[ -n "$package" ] || package="$repo/build/ps5/title/$title_id"
# Agents never launch the release install (AGENTS.md): refuse the selected
# identity, and a --package built as the release one, before any build/request.
ps5_agent_guard_identity "$title_id"
package_id="$(ps5_package_title_id "$package")" || package_id=""
[ -z "$package_id" ] || ps5_agent_guard_identity "$package_id"
[ -d "$package" ] && [ -f "$package/eboot.bin" ] ||
    { echo "run-vr-hello: no title package at $package; make -C platform/ps5/vr_hello vr-hello-title" >&2; exit 2; }

title_sh="$here/title.sh"
klog_sh="$here/klog.sh"
capture_py="$here/title-capture.py"

out_dir="$(ps5_data_dir)/runs/console"
log="$out_dir/$(date -u +%Y%m%dT%H%M%SZ)-vr_hello-title.log"

if [ "$dry" = 1 ]; then
    note "title route: $title_id ($package), 30 s run compiled in, ${window}s capture window"
    note "no input file: the pattern and the run length are compiled in"
    note "install replaces the viewer's eboot in the one title $title_id; run-viewer.sh installs the viewer again"
    note "1. install (skipped when title.sh status already verifies the package)"
    "$title_sh" status --dry-run "$package" | sed 's/^/  /'
    "$title_sh" install --dry-run "$package"
    note "2. launch"
    "$title_sh" launch --dry-run "$package" | sed 's/^/  /'
    note "3. stream klog -> $log until /^vrhello: (done, exit|FAIL)/ or ${window}s"
    note "4. stop (a no-op after a normal end)"
    "$title_sh" stop --dry-run "$package" | sed 's/^/  /'
    note "dry run: nothing was sent"
    exit 0
fi

: "${PS5_HOST:?run-vr-hello: set PS5_HOST or add PS5_HOST=... to .env.local}"

if "$title_sh" status "$package" >/dev/null 2>&1; then
    note "package already installed and verified (title.sh status); skipping install"
else
    note "installing $package ($(du -sk "$package" | awk '{printf "%.1f MiB", $1/1024}'))"
    "$title_sh" install "$package" || exit 4
fi
note "$title_id now runs the vr_hello eboot in place of the viewer's:"
note "  reinstall the viewer afterwards with platform/ps5/scripts/run-viewer.sh"

mkdir -p "$out_dir"
note "log $log"

capture_rc=0
PS5_RUN_TAG=run-vr-hello PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" \
PS5_RUN_SECONDS="$window" PS5_RUN_OUT="$log" \
PS5_RUN_TITLE_SH="$title_sh" PS5_RUN_PACKAGE="$package" PS5_RUN_KLOG="$klog_sh" \
PS5_RUN_END='vrhello: (?:done, exit|FAIL)' \
python3 "$capture_py" || capture_rc=$?

stop_rc=0
"$title_sh" stop "$package" || stop_rc=$?

if [ "$capture_rc" -ne 0 ]; then
    note "the title did not finish (exit $capture_rc); capture in $log"
    exit "$capture_rc"
fi
if [ "$stop_rc" -ne 0 ]; then
    note "the title finished but the title did not stop (title.sh exit $stop_rc)"
    exit 5
fi
note "done: the title reached its end line; capture in $log"
exit 0
