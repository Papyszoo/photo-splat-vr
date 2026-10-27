#!/usr/bin/env bash
# One-command console dev loop: make sure the log stream is up, deploy an ELF,
# then capture the console log.
#
#   platform/ps5/scripts/run.sh platform/ps5/hello/hello.elf
#   platform/ps5/scripts/run.sh platform/ps5/hello/hello.elf 20
#
# Steps: `klog.sh --ensure` (starts the klogsrv payload through ProsperoMgr
# when 3232 is closed), connect to the log, `deploy.sh <elf>` (ProsperoMgr
# upload + launch, so elfldr stays loopback-only), then stream for N seconds
# (default PS5_RUN_SECONDS=12) to stdout and to
# ${SPLAT_DATA_DIR:-<repo parent>}/runs/console/<UTC timestamp>-<name>.log.
#
# Exit status: 0 the payload's start line appeared, 1 it did not, 2 usage,
# 3 the log stream is unavailable, 4 the deploy failed, 5 no log lines at all.
#
# The "start line" is the first log line that names the payload: the ELF's stem
# as a case-insensitive extended regex (hello.elf -> hello). Kernel log lines
# carry the payload's process name, and a payload normally prints its own name
# in its first line. ProsperoMgr's own "POST /api/payloads/..." bookkeeping
# lines about the deploy are ignored, so "name=hello.elf" cannot count.
# PS5_RUN_READY=<extended regex> replaces the pattern entirely.
#
# Settings come from the environment or the repo's gitignored .env.local:
#   PS5_HOST, PS5_KLOG_PORT (3232), PS5_DEPLOY_METHOD (prospero),
#   PS5_PROSPERO_PORT (7070), PS5_RUN_SECONDS (12)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    cat <<'EOF'
usage: run.sh PAYLOAD.elf [SECONDS]

  PAYLOAD.elf  ELF to upload and launch through ProsperoMgr
  SECONDS      how long to capture the console log (default PS5_RUN_SECONDS=12)

Exit status: 0 start line seen, 1 not seen, 2 usage, 3 no log stream,
4 deploy failed, 5 the log stayed empty.
EOF
}

case "${1:-}" in
    ''|-h|--help) usage; exit 2 ;;
    -*) usage; exit 2 ;;
esac

elf=$1
shift
case "${1:-}" in
    '') ;;
    -*) usage; exit 2 ;;
    *) seconds=$1; shift ;;
esac
[ "$#" -eq 0 ] || { usage >&2; exit 2; }

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"
ps5_env_load

: "${PS5_HOST:?run: set PS5_HOST or add PS5_HOST=... to .env.local}"
[ -f "$elf" ] || ps5_die "run: no such file: $elf"

port="${PS5_KLOG_PORT:-3232}"
seconds="${seconds:-${PS5_RUN_SECONDS:-12}}"
case "$seconds" in
    ''|*[!0-9]*) ps5_die "run: SECONDS must be a whole number, got '$seconds'" ;;
esac
[ "$seconds" -gt 0 ] || ps5_die "run: SECONDS must be at least 1"

name="$(basename -- "$elf")"
stem="${name%.*}"
out_dir="$(ps5_data_dir)/runs/console"
mkdir -p "$out_dir"
out="$out_dir/$(date -u +%Y%m%dT%H%M%SZ)-$stem.log"

PS5_RUN_HOST="$PS5_HOST" PS5_RUN_PORT="$port" PS5_RUN_SECONDS="$seconds" \
PS5_RUN_ELF="$elf" PS5_RUN_NAME="$name" PS5_RUN_STEM="$stem" \
PS5_RUN_OUT="$out" PS5_RUN_READY="${PS5_RUN_READY:-}" \
PS5_RUN_KLOG="$here/klog.sh" PS5_RUN_DEPLOY="$here/deploy.sh" \
python3 - <<'PY'
import os, re, signal, socket, subprocess, sys, time

def note(msg):
    sys.stderr.write("run: %s\n" % msg)
    sys.stderr.flush()

def fail(msg, code):
    note(msg)
    sys.exit(code)

host = os.environ["PS5_RUN_HOST"]
port = int(os.environ["PS5_RUN_PORT"])
seconds = float(os.environ["PS5_RUN_SECONDS"])
elf = os.environ["PS5_RUN_ELF"]
name = os.environ["PS5_RUN_NAME"]
stem = os.environ["PS5_RUN_STEM"]
out_path = os.environ["PS5_RUN_OUT"]
ready = os.environ["PS5_RUN_READY"]
klog = os.environ["PS5_RUN_KLOG"]
deploy = os.environ["PS5_RUN_DEPLOY"]

# --- 1. log stream (starts klogsrv through ProsperoMgr when 3232 is closed) --
ensure = subprocess.run([klog, "--ensure"])
if ensure.returncode != 0:
    fail("no log stream: klog.sh --ensure exited %d" % ensure.returncode, 3)

# --- 2. connect before deploying: klogsrv forwards live only ------------------
try:
    sock = socket.create_connection((host, port), timeout=5)
except OSError as error:
    fail("cannot connect to the log stream %s:%d: %s" % (host, port, error), 3)
sock.settimeout(0.25)
note("log %s" % out_path)
note("streaming %s:%d for %ds" % (host, port, int(seconds)))

log_file = open(out_path, "wb")

# --- 3. deploy (its stdout goes to stderr, so stdout is only the log) --------
note("deploy %s" % name)
try:
    done = subprocess.run([deploy, elf], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
except OSError as error:
    log_file.close()
    sock.close()
    fail("cannot run %s: %s" % (deploy, error), 4)
for line in done.stdout.decode("utf-8", "replace").splitlines():
    sys.stderr.write("  deploy| %s\n" % line)
sys.stderr.flush()
if done.returncode != 0:
    log_file.close()
    sock.close()
    fail("deploy failed (exit %d); nothing to read from the payload" % done.returncode, 4)

# --- 4. stream ---------------------------------------------------------------
pattern = re.compile(ready if ready else re.escape(stem), re.IGNORECASE)
lines = []          # every complete line seen so far
pending = ""        # trailing bytes without a newline yet
found = None
total = 0
stdout = sys.stdout.buffer
deadline = time.time() + seconds

def matches(line):
    # Ignore the deploy path's own bookkeeping: "POST /api/payloads/upload
    # name=hello.elf" names the payload but is ProsperoMgr talking, not the
    # payload's first line.
    if "api/payloads" in line:
        return False
    return pattern.search(line) is not None

def stop(signum, frame):
    raise SystemExit(130)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

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
            lines.append(line)
            if found is None and matches(line):
                found = line.strip()
finally:
    sock.close()
    log_file.close()
    if found is None and pending and matches(pending):
        found = pending.strip()

# --- 5. verdict --------------------------------------------------------------
if found is not None:
    note("start line: %s" % found)
    note("captured %d bytes to %s" % (total, out_path))
    sys.exit(0)
if total == 0:
    note("no log lines at all in %ds (is /dev/klog alive? see klog.sh --ensure)" % int(seconds))
    note("captured 0 bytes to %s" % out_path)
    sys.exit(5)
note("payload start line never appeared in %ds (pattern: %s)" % (int(seconds), pattern.pattern))
note("captured %d bytes to %s" % (total, out_path))
sys.exit(1)
PY
