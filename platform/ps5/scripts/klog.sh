#!/usr/bin/env bash
# Streams the console kernel log, starting the log server first when needed.
#
#   platform/ps5/scripts/klog.sh                       # live, to the terminal
#   platform/ps5/scripts/klog.sh /path/to/console.log  # live, and to a file
#   platform/ps5/scripts/klog.sh --ensure              # start it, do not stream
#   PS5_KLOG_SECONDS=20 platform/ps5/scripts/klog.sh out.log   # stop after 20 s
#
# The default source is the **klogsrv** payload on port 3232. When nothing
# listens there, the script starts klogsrv through ProsperoMgr: `POST
# /api/payloads/launch` streams the stored ELF to elfldr on the console's own
# loopback (127.0.0.1:9021), so port 9021 never has to be reachable from this
# machine. ProsperoMgr ships klogsrv as a bundled payload, so nothing needs
# uploading. Only that one POST (plus GET /api/status and GET /api/payloads to
# check first) is ever made; the rest of the unauthenticated API is not used.
#
# etaHEN's klog forwarder serves the same raw /dev/klog text on 9081; set
# PS5_KLOG_PORT=9081 to read it. klog.sh only ever starts klogsrv, never etaHEN.
#
# Settings come from the environment or the repo's gitignored .env.local:
#   PS5_HOST, PS5_KLOG_PORT (3232), PS5_KLOGSRV_PORT (3232),
#   PS5_KLOGSRV_PAYLOAD (klogsrv.elf), PS5_PROSPERO_PORT (7070)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    cat <<'EOF'
usage: klog.sh [--ensure] [FILE]

  FILE        also append the log to FILE (stdout always gets it)
  --ensure    start the log server if needed, do not stream
  -h, --help  this text

Settings (environment or .env.local): PS5_HOST, PS5_KLOG_PORT (3232),
PS5_KLOGSRV_PORT (3232), PS5_KLOGSRV_PAYLOAD (klogsrv.elf), PS5_PROSPERO_PORT.
EOF
}

ensure_only=0
out=
for arg in "$@"; do
    case "$arg" in
        -e|--ensure) ensure_only=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "klog: unknown option: $arg" >&2; exit 2 ;;
        *)
            [ -z "$out" ] || { echo "klog: at most one output file" >&2; exit 2; }
            out=$arg
            ;;
    esac
done

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"
ps5_env_load

: "${PS5_HOST:?klog: set PS5_HOST or add PS5_HOST=... to .env.local}"
port="${PS5_KLOG_PORT:-3232}"
klogsrv_port="${PS5_KLOGSRV_PORT:-3232}"
klogsrv_payload="${PS5_KLOGSRV_PAYLOAD:-klogsrv.elf}"
prospero_port="${PS5_PROSPERO_PORT:-7070}"
seconds="${PS5_KLOG_SECONDS:-0}"

# klogsrv is listed as an enabled payload in ProsperoMgr's payload directory.
# 0 = present, 4 = present but disabled, 1 = not listed, 2 = no usable answer.
klogsrv_listed() {
    python3 -c '
import json, sys
try:
    payloads = json.load(sys.stdin).get("payloads") or []
except Exception:
    sys.exit(2)
for p in payloads:
    if p.get("name") == sys.argv[1]:
        sys.exit(0 if p.get("enabled", True) else 4)
sys.exit(1)
' "$klogsrv_payload"
}

# Starts klogsrv through ProsperoMgr when the log port is closed. Returns 0 if
# the port is (or becomes) open, 3 with an explanation on stderr otherwise.
ensure_stream() {
    ps5_port_open "$PS5_HOST" "$port" 2 && return 0

    if [ "$port" != "$klogsrv_port" ]; then
        cat >&2 <<EOF
klog: nothing is listening on $PS5_HOST:$port.
That is not the klogsrv port ($klogsrv_port), so klog.sh will not start
anything there. Either start that log source on the console yourself, or
unset PS5_KLOG_PORT to use klogsrv (klog.sh starts it through ProsperoMgr).
EOF
        return 3
    fi

    local body
    if ! body=$(ps5_http GET /api/status none ""); then
        cat >&2 <<EOF
klog: $PS5_HOST:$port is closed and ProsperoMgr did not answer on
$PS5_HOST:$prospero_port, so klog.sh cannot start klogsrv.
On the console:
  - launch the klogsrv payload: ProsperoMgr -> Payloads -> klogsrv.elf -> Launch
  - or start etaHEN with its klog server on 9081 and use PS5_KLOG_PORT=9081
Nothing was written to any port except the probes above.
EOF
        return 3
    fi
    if [ "$(printf '%s' "$body" | ps5_json_field name)" != "Prospero Manager" ]; then
        echo "klog: the service on $PS5_HOST:$prospero_port is not ProsperoMgr; not launching anything" >&2
        return 3
    fi

    if ! body=$(ps5_http GET /api/payloads none ""); then
        echo "klog: GET /api/payloads failed; cannot check for $klogsrv_payload" >&2
        return 3
    fi
    local listed=0
    printf '%s' "$body" | klogsrv_listed || listed=$?
    case "$listed" in
        0) ;;
        4)
            echo "klog: $klogsrv_payload is disabled in ProsperoMgr; enable it there (Payloads), then retry" >&2
            return 3
            ;;
        *)
            cat >&2 <<EOF
klog: ProsperoMgr does not list $klogsrv_payload in /data/ProsperoMgr/payloads
or /data/ProsperoMgr/bundled, so there is nothing to launch. Upload a klogsrv
build there (Payloads -> Upload), or start klogsrv another way, then retry.
EOF
            return 3
            ;;
    esac

    echo "klog: $PS5_HOST:$port is closed; starting $klogsrv_payload through ProsperoMgr ($PS5_HOST:$prospero_port)" >&2
    local launch_json
    launch_json=$(python3 -c 'import json, sys; print(json.dumps({"name": sys.argv[1]}))' "$klogsrv_payload")
    if ! body=$(ps5_http POST /api/payloads/launch json "$launch_json"); then
        echo "klog: launch of $klogsrv_payload failed (see the error above)" >&2
        return 3
    fi
    printf 'klog: ProsperoMgr answered %s\n' "$body" >&2

    local i=0
    while [ "$i" -lt 40 ]; do
        if ps5_port_open "$PS5_HOST" "$port" 1; then
            echo "klog: $klogsrv_payload is listening on $PS5_HOST:$port" >&2
            return 0
        fi
        sleep 0.25
        i=$((i + 1))
    done
    echo "klog: launched $klogsrv_payload, but $PS5_HOST:$port did not open within 10 s" >&2
    return 3
}

ensure_stream || exit $?

if [ "$ensure_only" = 1 ]; then
    echo "klog: log stream ready on $PS5_HOST:$port" >&2
    exit 0
fi

if [ "$seconds" = 0 ]; then
    echo "klog: $PS5_HOST:$port" >&2
else
    echo "klog: $PS5_HOST:$port (${seconds}s)" >&2
fi
[ -z "$out" ] || mkdir -p "$(dirname -- "$out")"

stream() {
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$PS5_HOST" "$port" "$seconds" <<'PY'
import signal, socket, sys, time

host, port, seconds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])

# Ctrl-C / kill must end the stream quietly; tee still gets a clean EOF.
def stop(signum, frame):
    raise SystemExit(0)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)

sock = socket.create_connection((host, port), timeout=5)
# klogsrv only starts forwarding /dev/klog once a client is connected, so this
# line is the "live" marker run.sh waits for before deploying.
sys.stderr.write("klog: streaming %s:%d\n" % (host, port))
sys.stderr.flush()
sock.settimeout(1.0)
deadline = time.time() + seconds if seconds > 0 else None
out = sys.stdout.buffer
try:
    while True:
        if deadline is not None and time.time() >= deadline:
            break
        try:
            data = sock.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        out.write(data)
        out.flush()
except BrokenPipeError:
    pass
finally:
    sock.close()
PY
    else
        # bash /dev/tcp fallback: no duration support, Ctrl-C to stop.
        [ "$seconds" = "0" ] || { echo "klog: PS5_KLOG_SECONDS needs python3" >&2; exit 2; }
        exec 3<>"/dev/tcp/$PS5_HOST/$port"
        cat <&3
    fi
}

if [ -n "$out" ]; then
    stream | tee -a "$out"
else
    stream
fi
