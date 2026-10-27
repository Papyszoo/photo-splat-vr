#!/usr/bin/env bash
# Deploys a payload ELF to the console.
#
#   platform/ps5/scripts/deploy.sh platform/ps5/hello/hello.elf
#
# Two methods, picked by PS5_DEPLOY_METHOD:
#
#   prospero (default)  Upload to ProsperoMgr (7070) and launch it from there.
#                       ProsperoMgr streams the file to elfldr on the console's
#                       own loopback (127.0.0.1:9021), so port 9021 never has to
#                       be reachable from this machine.
#   raw                 Stream the ELF straight to elfldr on $PS5_HOST:9021 and
#                       print the payload's stdout as it comes back.
#
# Settings come from the environment or the repo's gitignored .env.local:
#   PS5_HOST, PS5_DEPLOY_METHOD, PS5_PROSPERO_PORT (7070), PS5_PORT (9021),
#   PS5_KLOG_PORT (3232)
# A payload launched through ProsperoMgr does not return its stdout; its klog
# lines show up on PS5_KLOG_PORT (klogsrv, 3232) if it calls klog_puts().
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"
ps5_env_load

elf=${1:-}
[ -n "$elf" ] || ps5_die "usage: deploy.sh PAYLOAD.elf"
case "$elf" in -h|--help) ps5_die "usage: deploy.sh PAYLOAD.elf   (PS5_DEPLOY_METHOD=prospero|raw)";; esac
[ -f "$elf" ] || ps5_die "deploy: no such file: $elf"

: "${PS5_HOST:?deploy: set PS5_HOST or add PS5_HOST=... to .env.local}"
method="${PS5_DEPLOY_METHOD:-prospero}"
prospero_port="${PS5_PROSPERO_PORT:-7070}"
raw_port="${PS5_PORT:-9021}"
klog_port="${PS5_KLOG_PORT:-3232}"
timeout="${PS5_TIMEOUT:-60}"

say() { printf 'deploy: %s\n' "$*" >&2; }

# --- ProsperoMgr HTTP client -------------------------------------------------
# ps5_http and ps5_json_field live in ps5-env.sh; urlenc/json_name encode a
# payload name without relying on bash quoting.

# Percent-encode a query value / JSON-quote a payload name without bash escapes.
urlenc() { python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"; }
json_name() { python3 -c 'import json, sys; print(json.dumps({"name": sys.argv[1]}))' "$1"; }

deploy_prospero() {
    local name body
    name="$(basename -- "$elf")"
    case "$name" in
        *.elf|*.ELF|*.bin|*.BIN|*.self|*.SELF) ;;
        *) ps5_die "deploy: prospero uploads only .elf/.bin/.self names (ProsperoMgr rejects '$name')" ;;
    esac
    case "$name" in
        */*|*..*) ps5_die "deploy: payload name must not contain '/' or '..': $name" ;;
    esac

    say "method prospero: $elf -> ProsperoMgr $PS5_HOST:$prospero_port (elfldr on the console's loopback)"

    body=$(ps5_http GET /api/status none "") || {
        cat >&2 <<EOF
deploy: no HTTP answer from ProsperoMgr on $PS5_HOST:$prospero_port.
On the console, launch ProsperoMgr (it binds 7070), then retry.
Nothing was sent to any other port.
EOF
        exit 3
    }
    printf '%s\n' "$body"
    [ "$(printf '%s' "$body" | ps5_json_field name)" = "Prospero Manager" ] || {
        say "the service on $prospero_port is not ProsperoMgr; refusing to continue"
        exit 3
    }

    body=$(ps5_http GET /api/elfldr/status none "") || {
        say "GET /api/elfldr/status failed; refusing to upload or launch"
        exit 3
    }
    printf '%s\n' "$body"
    [ "$(printf '%s' "$body" | ps5_json_field reachable)" = "true" ] || {
        cat >&2 <<EOF
deploy: ProsperoMgr cannot reach elfldr on the console's loopback (127.0.0.1:9021).
An ELF loader must be running on the console before anything can be launched:
  - autoload elfldr-ps5.elf (Y2JB / BD-JB autoloader, first entry of autoload.txt), or
  - launch it from Payload Manager (ps5-payload-manager).
Then run this script again. Nothing was uploaded.
EOF
        exit 3
    }

    say "upload POST /api/payloads/upload?name=$name"
    body=$(ps5_http POST "/api/payloads/upload?name=$(urlenc "$name")" file "$elf") || {
        say "upload failed"
        exit 4
    }
    printf '%s\n' "$body"

    say "launch POST /api/payloads/launch"
    body=$(ps5_http POST /api/payloads/launch json "$(json_name "$name")") || {
        say "launch failed (see the error above)"
        exit 4
    }
    printf '%s\n' "$body"

    say "launched; payload stdout is not returned by this path — read the payload's klog lines on $PS5_HOST:$klog_port (klog.sh)"
    [ "$(printf '%s' "$body" | ps5_json_field ok)" = "true" ] || exit 4
}

# --- raw elfldr --------------------------------------------------------------
deploy_raw() {
    say "method raw: $elf -> elfldr $PS5_HOST:$raw_port"
    # No port probe first: a bare TCP connect is an empty payload to elfldr.
    if command -v socat >/dev/null 2>&1; then
        socat -t "$timeout" - "TCP:$PS5_HOST:$raw_port" < "$elf"
    else
        say "socat not found (brew install socat / apt install socat); using python3"
        PS5_ELF="$elf" PS5_DEPLOY_HOST="$PS5_HOST" PS5_DEPLOY_PORT="$raw_port" \
        PS5_DEPLOY_TIMEOUT="$timeout" python3 - <<'PY'
import os, socket, sys, threading

with open(os.environ["PS5_ELF"], "rb") as f:
    elf = f.read()
s = socket.create_connection((os.environ["PS5_DEPLOY_HOST"], int(os.environ["PS5_DEPLOY_PORT"])), timeout=10)
s.settimeout(float(os.environ["PS5_DEPLOY_TIMEOUT"]))
s.sendall(elf)
s.shutdown(socket.SHUT_WR)  # elfldr loads on EOF; its output follows on the same socket

def drain():
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            return
        if not b:
            return
        sys.stdout.buffer.write(b)
        sys.stdout.buffer.flush()

t = threading.Thread(target=drain)
t.start()
t.join()
PY
    fi || {
        cat >&2 <<EOF
deploy: could not send to $PS5_HOST:$raw_port (elfldr).
Reach elfldr either way:
  - PS5_DEPLOY_METHOD=prospero (default): ProsperoMgr on 7070 streams to the
    console's own loopback elfldr, no LAN port needed. Or
  - start elfldr on the console (Payload Manager / autoloader), and make 9021
    reachable from this machine for the raw method.
EOF
        exit 3
    }
}

case "$method" in
    prospero) deploy_prospero ;;
    raw|elfldr) deploy_raw ;;
    *) ps5_die "deploy: unknown PS5_DEPLOY_METHOD='$method' (prospero|raw)" ;;
esac
