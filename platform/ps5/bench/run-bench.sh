#!/usr/bin/env bash
# One command per Spike A bench: the payload route (original) or the homebrew
# title route (the only one that can measure GPU memory: a payload has 0 B of
# direct memory, so RADV gives it no heaps -- docs/research/spike-b-memory.md
# sections 1-2).
#
#   platform/ps5/bench/run-bench.sh op_audit 1200             # payload (as before)
#   platform/ps5/bench/run-bench.sh --title op_audit          # title, 1200 s window
#   platform/ps5/bench/run-bench.sh --title sharp_bench 1800
#   platform/ps5/bench/run-bench.sh --title --dry-run op_audit
#
# Title route, in order: write the selector BENCH=<name> (plus any settings from
# --env FILE) to <dataDir>/bench/bench.env, install the package if
# title.sh status does not already pass, launch it, stream klog to
# $SPLAT_DATA_DIR/runs/console/<UTC>-<name>-title.log until that bench's end
# line, then stop the title. There is ONE title package for all three benches,
# and it is the same title as the viewer and vr_hello (id from
# platform/ps5/title/identities.json, the dev identity), and bench.env selects the
# bench:
# ProsperoMgr launches a title id and passes no arguments, so the selector has
# to be a file. Why one title and not three: docs/research/spike-a.md 2.4, and
# docs/PLAN.md "One app on the console". Because it is the same title,
# installing this eboot replaces the viewer's in place: the run says so, and
# run-viewer.sh installs the viewer again.
#
# --env FILE appends the entries of a local bench.env template (driver settings
# such as GGML_VK_DISABLE_COOPMAT=1) after BENCH=; a BENCH= line in the template
# is replaced, not duplicated. Without --env the remote file is BENCH= only.
#
# Payload route: sets PS5_RUN_READY and PS5_HTTP_TIMEOUT and hands over to
# run.sh, exactly as before (0 start line seen, 1 not, 2 usage, 3 no log
# stream, 4 deploy failed, 5 empty capture).
#
# Title route exit: 0 end line seen, 1 not seen in the window, 2 usage or the
# package/file is missing, 3 no log stream, 4 put/install/launch failed, 5 the
# capture stayed empty or the title did not stop.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    cat <<'EOF'
usage: run-bench.sh [--title|--payload] [--dry-run] [--env FILE] BENCH [SECONDS]

  BENCH        op_audit, da2_bench or sharp_bench
  SECONDS      capture window (title default: op_audit 1200, da2_bench 300,
               sharp_bench 900; payload default: run.sh's 12)
  --title      run as the homebrew title (the dev identity, the one app),
               selected through <dataDir>/bench/bench.env
  --payload    run the bench ELF through run.sh (the original route)
  --dry-run    print every step and request; send nothing
  --env FILE   local bench.env template for the title route (driver settings)

Title route: needs the package (make -C platform/ps5/bench bench-title),
ProsperoMgr, ShadowMountPlus, a signed-in foreground user and the klog stream.
EOF
}

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=../scripts/ps5-env.sh
. "$here/../scripts/ps5-env.sh"
ps5_env_load

mode="payload"
dry=0
env_file=""
name=""
seconds=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --title) mode="title" ;;
        --payload) mode="payload" ;;
        --dry-run) dry=1 ;;
        --env)
            shift
            [ "$#" -gt 0 ] || { usage >&2; exit 2; }
            env_file="$1" ;;
        --env=*) env_file="${1#--env=}" ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "run-bench: unknown option '$1'" >&2; usage >&2; exit 2 ;;
        *)
            if [ -z "$name" ]; then
                name="$1"
            elif [ -z "$seconds" ]; then
                seconds="$1"
            else
                usage >&2
                exit 2
            fi ;;
    esac
    shift
done

case "$name" in
    op_audit|da2_bench|sharp_bench) ;;
    '') usage >&2; exit 2 ;;
    *) echo "run-bench: unknown bench '$name'" >&2; exit 2 ;;
esac
if [ -n "$seconds" ]; then
    case "$seconds" in
        *[!0-9]*) echo "run-bench: SECONDS must be a whole number, got '$seconds'" >&2; exit 2 ;;
    esac
fi

note() { printf 'run-bench: %s\n' "$*" >&2; }

if [ "$mode" = "payload" ] && [ -n "$env_file" ]; then
    echo "run-bench: --env is for the title route (--title); the payload route reads the console's bench.env" >&2
    exit 2
fi

# ---- payload route (unchanged) ----------------------------------------------
if [ "$mode" = "payload" ]; then
    if [ "$dry" = 1 ]; then
        elf="$here/$name.stripped.elf"
        [ -f "$elf" ] || elf="$here/$name.elf"
        note "payload route: PS5_RUN_READY='$name:' PS5_HTTP_TIMEOUT=300 run.sh $elf ${seconds:-12}"
        exit 0
    fi
    elf="$here/$name.stripped.elf"
    [ -f "$elf" ] || elf="$here/$name.elf"
    [ -f "$elf" ] || { echo "run-bench: no $name ELF; make -C $here" >&2; exit 2; }

    export PS5_RUN_READY="${PS5_RUN_READY:-$name:}"
    export PS5_HTTP_TIMEOUT="${PS5_HTTP_TIMEOUT:-300}"

    exec "$here/../scripts/run.sh" "$elf" ${seconds:+"$seconds"}
fi

# ---- title route ------------------------------------------------------------
title_sh="$here/../scripts/title.sh"
klog_sh="$here/../scripts/klog.sh"
title_id="$(ps5_title_id)"
# Agents never launch the release install (AGENTS.md).
ps5_agent_guard_identity "$title_id"
data_root="$(ps5_title_data_dir)"
package="$repo/build/ps5/title/$title_id"
remote_env="$data_root/bench/bench.env"

[ -d "$package" ] && [ -f "$package/eboot.bin" ] ||
    { echo "run-bench: no title package at $package; make -C $here bench-title" >&2; exit 2; }
if [ -n "$env_file" ] && [ ! -f "$env_file" ]; then
    echo "run-bench: no such --env file: $env_file" >&2
    exit 2
fi

# The capture window: the previous per-bench values from spike-a.md section 7.
if [ -z "$seconds" ]; then
    case "$name" in
        op_audit) seconds=1200 ;;
        da2_bench) seconds=300 ;;
        sharp_bench) seconds=900 ;;
    esac
fi

# bench.env is the title's only input (the launch route passes no arguments):
# the selector first, then whatever driver settings the caller's template has.
tmp_env="$(mktemp "${TMPDIR:-/tmp}/ps5splat-bench-env.XXXXXX")"
trap 'rm -f "$tmp_env"' EXIT
{
    printf '# written by run-bench.sh --title at %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'BENCH=%s\n' "$name"
    if [ -n "$env_file" ]; then
        # A BENCH= line in the template would fight the selector above.
        grep -v -E '^[[:space:]]*BENCH[[:space:]]*=' "$env_file" || true
    fi
} >"$tmp_env"

out_dir="$(ps5_data_dir)/runs/console"
log="$out_dir/$(date -u +%Y%m%dT%H%M%SZ)-$name-title.log"

if [ "$dry" = 1 ]; then
    note "title route: $title_id ($package), bench $name, capture window ${seconds}s"
    note "install replaces the viewer's eboot in the one title $title_id; run-viewer.sh installs the viewer again"
    note "1. selector: $remote_env"
    printf '  body %s bytes:\n' "$(wc -c <"$tmp_env" | tr -d ' ')"
    sed 's/^/    /' "$tmp_env"
    "$title_sh" put --dry-run "$tmp_env" "$remote_env"
    note "2. install (skipped when title.sh status already verifies the package)"
    "$title_sh" status --dry-run "$package" | sed 's/^/  /'
    "$title_sh" install --dry-run "$package"
    note "3. launch"
    "$title_sh" launch --dry-run "$package" | sed 's/^/  /'
    note "4. stream klog -> $log until /^$name: (done, exit|WATCHDOG abandoning)/ or ${seconds}s"
    note "5. stop"
    "$title_sh" stop --dry-run "$package" | sed 's/^/  /'
    note "dry run: nothing was sent"
    exit 0
fi

: "${PS5_HOST:?run-bench: set PS5_HOST or add PS5_HOST=... to .env.local}"

# The selector is not part of the package: it changes per run.
note "writing $remote_env (BENCH=$name)"
"$title_sh" put "$tmp_env" "$remote_env" || exit 4

if "$title_sh" status "$package" >/dev/null 2>&1; then
    note "package already installed and verified (title.sh status); skipping install"
else
    note "installing $package"
    "$title_sh" install "$package" || exit 4
fi
note "$title_id now runs the bench eboot in place of the viewer's:"
note "  reinstall the viewer afterwards with platform/ps5/scripts/run-viewer.sh"

mkdir -p "$out_dir"
note "log $log"

# klog first, then launch (klogsrv forwards live only, it replays nothing), then
# read until the bench's end line. The stream is one socket, as run.sh uses.
capture_rc=0
PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="${PS5_KLOG_PORT:-3232}" \
PS5_RUN_SECONDS="$seconds" PS5_RUN_OUT="$log" PS5_RUN_BENCH="$name" \
PS5_RUN_TITLE_SH="$title_sh" PS5_RUN_PACKAGE="$package" PS5_RUN_KLOG="$klog_sh" \
python3 - <<'PY' || capture_rc=$?
import os, re, signal, socket, subprocess, sys, time

def note(msg):
    sys.stderr.write("run-bench: %s\n" % msg)
    sys.stderr.flush()

def fail(msg, code):
    note(msg)
    sys.exit(code)

host = os.environ["PS5_RUN_HOST"]
port = int(os.environ["PS5_RUN_PORT"])
seconds = float(os.environ["PS5_RUN_SECONDS"])
out_path = os.environ["PS5_RUN_OUT"]
bench = os.environ["PS5_RUN_BENCH"]
title_sh = os.environ["PS5_RUN_TITLE_SH"]
package = os.environ["PS5_RUN_PACKAGE"]
klog = os.environ["PS5_RUN_KLOG"]

# The bench's own last line, the watchdog's abandon line, or the title's exit
# line if a bench somehow returned without one.
end = re.compile(r"(?:%s: (?:done, exit|WATCHDOG abandoning|abandoning the process)"
                 r"|bench_title: .* done, exit)" % re.escape(bench))

ensure = subprocess.run([klog, "--ensure"])
if ensure.returncode != 0:
    fail("no log stream: klog.sh --ensure exited %d" % ensure.returncode, 3)

try:
    sock = socket.create_connection((host, port), timeout=5)
except OSError as error:
    fail("cannot connect to the log stream %s:%d: %s" % (host, port, error), 3)
sock.settimeout(0.25)
note("streaming %s:%d for up to %ds (log %s)" % (host, port, int(seconds), out_path))
log_file = open(out_path, "wb")

note("launch %s" % package)
launch = subprocess.run([title_sh, "launch", package], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
for line in launch.stdout.decode("utf-8", "replace").splitlines():
    sys.stderr.write("  launch| %s\n" % line)
sys.stderr.flush()
if launch.returncode != 0:
    log_file.close()
    sock.close()
    fail("launch failed (title.sh exit %d)" % launch.returncode, 4)

def stop(signum, frame):
    raise SystemExit(130)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

found = None
pending = ""
total = 0
stdout = sys.stdout.buffer
deadline = time.time() + seconds
try:
    while time.time() < deadline:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data:
            break
        stdout.write(data)
        stdout.flush()
        log_file.write(data)
        log_file.flush()
        total += len(data)
        pending += data.decode("utf-8", "replace")
        *lines_done, pending = pending.split("\n")
        for line in lines_done:
            if found is None and end.search(line):
                found = line.strip()
finally:
    sock.close()
    log_file.close()
    if found is None and pending and end.search(pending):
        found = pending.strip()

if found is not None:
    note("end line: %s" % found)
    note("captured %d bytes to %s" % (total, out_path))
    sys.exit(0)
if total == 0:
    note("no log lines at all in %ds (is /dev/klog alive? see klog.sh --ensure)" % int(seconds))
    sys.exit(5)
note("no end line for %s in %ds (captured %d bytes to %s)" % (bench, int(seconds), total, out_path))
sys.exit(1)
PY

# The title ends itself (LoadExec at the end, or the quit watch if it is still
# in a bench); stop never kills.
stop_rc=0
"$title_sh" stop "$package" || stop_rc=$?

if [ "$capture_rc" -ne 0 ]; then
    note "bench did not finish (exit $capture_rc); capture in $log"
    exit "$capture_rc"
fi
if [ "$stop_rc" -ne 0 ]; then
    note "the benchmark finished but the title did not stop (title.sh exit $stop_rc)"
    exit 5
fi
note "done: $name passed the end line; capture in $log"
exit 0
