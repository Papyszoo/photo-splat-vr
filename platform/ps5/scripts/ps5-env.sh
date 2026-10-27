#!/usr/bin/env bash
# Shared settings + probe helpers for platform/ps5/scripts/*.sh. Source it; do
# not run it. Bash 3.2 compatible (macOS /bin/bash): no ${!var}, no arrays.
#
# Settings come from the environment first, then from the repo's gitignored
# .env.local. Every PS5_* key in that file is exported:
#
#   PS5_HOST             console LAN address (required)
#   PS5_DEPLOY_METHOD    prospero (default) or raw
#   PS5_PROSPERO_PORT    ProsperoMgr dashboard/API, default 7070
#   PS5_PORT             elfldr port for the raw method, default 9021
#   PS5_KLOG_PORT        console log stream, default 3232 (klogsrv payload)
#   PS5_KLOGSRV_PORT     port klogsrv binds, default 3232 (launched when closed)
#   PS5_KLOGSRV_PAYLOAD  payload name to launch, default klogsrv.elf
#
# Identity and data paths come from platform/ps5/title/identities.json only
# (ps5_title_id, ps5_title_data_dir). Agents never drive the release identity
# and never touch its data folder: ps5_agent_guard_identity and ps5_guard_*
# refuse both before a request is built, printed or sent (AGENTS.md).
#
# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash

PS5_REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"

# ps5_env_load [FILE]: export PS5_* keys from FILE (default .env.local) that are
# not already in the environment. Accepts "KEY=value", "export KEY=value",
# single/double quotes around the value, and CRLF line endings.
ps5_env_load() {
    local file="${1:-${PS5_ENV_FILE:-$PS5_REPO_ROOT/.env.local}}"
    [ -f "$file" ] || return 0

    local line key value current
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in ''|'#'*) continue ;; esac
        line="${line#export }"
        case "$line" in *=*) ;; *) continue ;; esac
        key="${line%%=*}"
        value="${line#*=}"
        # Trim blanks around the key and the value.
        key="${key#"${key%%[![:space:]]*}"}"
        key="${key%"${key##*[![:space:]]}"}"
        value="${value#"${value%%[![:space:]]*}"}"
        value="${value%"${value##*[![:space:]]}"}"
        case "$value" in
            \"*\") value="${value#\"}"; value="${value%\"}" ;;
            \'*\') value="${value#\'}"; value="${value%\'}" ;;
        esac
        case "$key" in PS5_*) ;; *) continue ;; esac
        eval "current=\${$key:-}"
        [ -n "$current" ] && continue
        export "$key=$value"
    done < "$file"
}

# ps5_port_open HOST PORT [TIMEOUT_SECONDS]: true if a TCP connect succeeds.
# A connect sends no bytes. Uses python3 (portable timeout), falls back to
# bash's /dev/tcp where python3 is missing. Never uses nc -G (BSD-only).
ps5_port_open() {
    local host="$1" port="$2" timeout="${3:-3}"
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$host" "$port" "$timeout" <<'PY'
import socket, sys
host, port, timeout = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
s = socket.socket()
s.settimeout(timeout)
try:
    s.connect((host, port))
except OSError:
    sys.exit(1)
finally:
    s.close()
PY
        return $?
    fi
    (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null
}

# ps5_die MESSAGE...: print to stderr and exit 2 (usage / missing file).
ps5_die() {
    printf '%s\n' "$*" >&2
    exit 2
}

# ps5_data_dir: $SPLAT_DATA_DIR, else the repository's parent directory. Large
# artifacts (captures, logs) live there, never in the repo.
ps5_data_dir() {
    printf '%s\n' "${SPLAT_DATA_DIR:-$(cd -- "$PS5_REPO_ROOT/.." && pwd)}"
}

# ps5_identity_selector [NAME]: the identity.py selector for this command:
# TITLE_ID=PPSA… first, then NAME if given, then RELEASE=1, else the table's
# default (the dev identity). One meaning for every script and every build.
ps5_identity_selector() {
    if [ -n "${TITLE_ID:-}" ]; then
        printf -- '--title-id %s' "$TITLE_ID"
    elif [ -n "${1:-}" ]; then
        printf -- '--name %s' "$1"
    else
        # Same off-values as build-title.mk (RELEASE=no is a dev build there):
        # a shell run and its build must never choose different identities.
        case "${RELEASE:-0}" in
            0|no|false) printf -- '--name dev' ;;
            *) printf -- '--name release' ;;
        esac
    fi
}

# ps5_title_field FIELD [--name NAME|--title-id ID]: one field of one identity
# from the single table platform/ps5/title/identities.json (titleId, conceptId,
# contentId, titleName, dataDir, agentsMayRun, name).
ps5_title_field() {
    local field="$1"
    shift
    python3 "$PS5_REPO_ROOT/platform/ps5/title/identity.py" --field "$field" "$@"
}

# ps5_title_id [NAME]: the title id of one build identity (dev by default).
# TITLE_ID=PPSA… selects one by id and RELEASE=1 the release identity, so a
# shell run and a `make … RELEASE=1` build agree on build/ps5/title/<id> and
# /data/homebrew/<id>.
ps5_title_id() {
    python3 "$PS5_REPO_ROOT/platform/ps5/title/identity.py" \
        $(ps5_identity_selector "${1:-}") | awk '{print $1}'
}

# ps5_title_package [NAME]: build/ps5/title/<id> for the same identity.
ps5_title_package() {
    printf '%s/build/ps5/title/%s\n' "$PS5_REPO_ROOT" "$(ps5_title_id "${1:-}")"
}

# ps5_title_data_dir [NAME]: the console data folder of the selected identity.
# The title was built with the same value (SPLAT_DATA_ROOT), so a script writes
# where that title reads.
ps5_title_data_dir() {
    ps5_title_field dataDir $(ps5_identity_selector "${1:-}")
}

# ps5_package_title_id PACKAGE_DIR: the titleId in a package's generated
# param.json (empty when the folder or the file is not readable).
ps5_package_title_id() {
    python3 -c 'import json, sys
try:
    print(json.load(open(sys.argv[1]))["titleId"])
except Exception:
    raise SystemExit(1)' "$1/sce_sys/param.json" 2>/dev/null
}

# --- agents never run the release install or touch its data ------------------
# User decision (AGENTS.md): the console carries two installs, and agent scripts
# may drive only the dev one. Both facts come from identities.json ("agentsMayRun"
# and each identity's dataDir), never from a second copy here.

# ps5_private_data_dirs: every data folder agents must not touch, one per line.
# A table that cannot be read is an error, never "no private folder": the path
# guard below fails closed instead of letting a request through.
ps5_private_data_dirs() {
    python3 "$PS5_REPO_ROOT/platform/ps5/title/identity.py" --private-data-dirs
}

# ps5_agent_guard_identity [TITLE_ID]: refuse to launch or drive an identity the
# table marks as not for agent scripts. Empty TITLE_ID = the selected identity.
# Exits 2 before a request is built, printed or sent.
ps5_agent_guard_identity() {
    local id allowed
    if [ -n "${1:-}" ]; then
        id="$1"
    else
        id="$(ps5_title_id)"
    fi
    [ -n "$id" ] || ps5_die "no identity selected (TITLE_ID/RELEASE); nothing was sent"
    allowed="$(ps5_title_field agentsMayRun --title-id "$id" 2>/dev/null)" || allowed=""
    if [ "$allowed" != "true" ]; then
        ps5_die "refusing $id: agent scripts never launch the release install or act on it" \
                "(identities.json marks it agentsMayRun=false; its data folder is the user's private one)." \
                "Only the dev identity is allowed here. Nothing was sent."
    fi
}

# ps5_guard_remote_path PATH...: refuse any console path at or below a private
# data folder before a request is built, printed or sent.
ps5_guard_remote_path() {
    local path dir private
    # An unreadable table must not turn the guard off (see ps5_private_data_dirs).
    private="$(ps5_private_data_dirs)" ||
        ps5_die "cannot read platform/ps5/title/identities.json; refusing every console path until the table is readable. Nothing was sent."
    [ -n "$private" ] || return 0
    for path in "$@"; do
        [ -n "$path" ] || continue
        while IFS= read -r dir; do
            [ -n "$dir" ] || continue
            case "$path" in
                "$dir"|"$dir"/*)
                    ps5_die "refusing $path: $dir is the user's private data folder;" \
                            "no script in this repo may read, list, write or delete there. Nothing was sent." ;;
            esac
        done <<EOF
$private
EOF
    done
}

# ps5_guard_http_path PATH [BODY]: ps5_guard_remote_path for every remote path a
# request names — the ?path= query values and a JSON body's "path". Every
# console request in this repo goes through ps5_http or calls this directly (the
# dry-run printers in title.sh), so the guard cannot be bypassed by a new caller.
ps5_guard_http_path() {
    local raw="$1" body="${2:-}" decoded path
    decoded="$(PS5_GUARD_RAW="$raw" PS5_GUARD_BODY="$body" python3 - <<'PY' 2>/dev/null
import json, os, urllib.parse
raw = os.environ["PS5_GUARD_RAW"]
paths = []
for key, value in urllib.parse.parse_qsl(urllib.parse.urlsplit("http://x" + raw).query):
    if key == "path":
        paths.append(value)
body = os.environ.get("PS5_GUARD_BODY", "")
if body.startswith("{"):
    try:
        value = json.loads(body).get("path")
    except ValueError:
        value = None
    if isinstance(value, str):
        paths.append(value)
print("\n".join(paths))
PY
)" || ps5_die "cannot decode the paths of a console request; refusing to send it until the guard can run. Nothing was sent."
    [ -n "$decoded" ] || return 0
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        ps5_guard_remote_path "$path"
    done <<EOF
$decoded
EOF
}


# --- ProsperoMgr HTTP client -------------------------------------------------
# ps5_http METHOD PATH KIND [DATA]
#   KIND=none  DATA ignored
#   KIND=json  DATA is a JSON string, sent as application/json
#   KIND=file  DATA is a path, sent as application/octet-stream
# Body goes to stdout; transport errors and HTTP >= 400 are non-zero exits.
# Only well-formed HTTP requests are ever written to the socket. The whole
# ProsperoMgr API is unauthenticated, so callers stick to the five routes
# deploy.sh and klog.sh need: status, elfldr/status, payloads, payloads/upload
# and payloads/launch (docs/research/ps5-console.md section 7).
ps5_http() {
    # No request may name the user's private data folder: checked before the
    # socket is opened, and again in the dry-run printers' path helpers.
    ps5_guard_http_path "${2:-}" "${4:-}"
    PS5_HTTP_HOST="$PS5_HOST" PS5_HTTP_PORT="${PS5_PROSPERO_PORT:-7070}" \
    PS5_HTTP_METHOD="$1" PS5_HTTP_PATH="$2" \
    PS5_HTTP_KIND="$3" PS5_HTTP_DATA="${4:-}" \
    PS5_HTTP_TIMEOUT="${PS5_HTTP_TIMEOUT:-30}" \
    python3 - <<'PY'
import http.client, os, sys

kind = os.environ["PS5_HTTP_KIND"]
headers = {"Accept": "application/json"}
data = None
if kind == "json":
    data = os.environ["PS5_HTTP_DATA"].encode()
    headers["Content-Type"] = "application/json"
elif kind == "file":
    with open(os.environ["PS5_HTTP_DATA"], "rb") as f:
        data = f.read()
    headers["Content-Type"] = "application/octet-stream"

conn = http.client.HTTPConnection(
    os.environ["PS5_HTTP_HOST"], int(os.environ["PS5_HTTP_PORT"]),
    timeout=float(os.environ["PS5_HTTP_TIMEOUT"]))
try:
    try:
        conn.request(os.environ["PS5_HTTP_METHOD"], os.environ["PS5_HTTP_PATH"],
                     body=data, headers=headers)
        resp = conn.getresponse()
        payload = resp.read()
        status = resp.status
    except OSError as error:
        # No answer at all (refused, timed out): one line, no traceback.
        sys.stderr.write("%s: %s\n" % (type(error).__name__, error))
        sys.exit(1)
finally:
    conn.close()

sys.stdout.buffer.write(payload)
sys.stdout.buffer.flush()
if status >= 400:
    sys.stderr.write("\nHTTP %d\n" % status)
    sys.exit(1)
PY
}

# ps5_json_field KEY < body: print one scalar of a JSON object, empty if the
# key is absent or the body is not JSON.
ps5_json_field() {
    python3 -c '
import json, sys
try:
    value = json.load(sys.stdin).get(sys.argv[1])
except Exception:
    value = None
if isinstance(value, bool):
    value = "true" if value else "false"
print("" if value is None else value)
' "$1" || true
}

# --- /api/ftp reads: the URL/JSON encoding and the listing parse, once -------
# ui-test.sh and spike.sh both save viewer.env, list a remote directory and
# compare a sha256, so those live here instead of in each script. Every
# function takes DRY as its last argument: "1" prints the request it would
# send (to fd $PS5_DRY_FD, default 2 = stderr) and sends nothing.

# ps5_base_url: http://$PS5_HOST:<PS5_PROSPERO_PORT>, the prefix of a printed
# request line.
ps5_base_url() {
    printf 'http://%s:%s' "${PS5_HOST:-<PS5_HOST>}" "${PS5_PROSPERO_PORT:-7070}"
}

# ps5_url_encode VALUE: VALUE percent-encoded for ?path=.
ps5_url_encode() {
    python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"
}

# ps5_json_path PATH: the JSON body the ftp routes take, {"path": PATH}.
ps5_json_path() {
    python3 -c 'import json,sys;print(json.dumps({"path":sys.argv[1]}))' "$1"
}

# ps5_sha256 FILE: the file's sha256 hex (shasum, else sha256sum).
ps5_sha256() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        echo "no-sha256-tool"
    fi
}

# ps5_dry_line TEXT: one printed (dry-run) request line.
ps5_dry_line() {
    printf '%s\n' "$*" >&"${PS5_DRY_FD:-2}"
}

# ps5_ftp_get_text REMOTE LOCAL [DRY]: 0 when the console returned the file.
ps5_ftp_get_text() {
    local remote="$1" local_file="$2" dry="${3:-0}" body
    ps5_guard_remote_path "$remote"
    if [ "$dry" = 1 ]; then
        ps5_dry_line "  GET  $(ps5_base_url)/api/ftp/text?path=$(ps5_url_encode "$remote")"
        return 1
    fi
    body="$(ps5_http GET "/api/ftp/text?path=$(ps5_url_encode "$remote")" none "" 2>/dev/null)" || return 1
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    sys.stdout.write(json.load(sys.stdin).get("text", ""))
except Exception:
    sys.exit(1)
' >"$local_file" || return 1
}

# ps5_ftp_list DIR [DRY]: one "name<TAB>size<TAB>file|dir" line per entry,
# nothing when DIR cannot be listed. Directories are listed too (is_dir, size
# 0), so a caller can see a scene folder and not only its files.
ps5_ftp_list() {
    local dir="$1" dry="${2:-0}" body
    ps5_guard_remote_path "$dir"
    if [ "$dry" = 1 ]; then
        ps5_dry_line "  GET  $(ps5_base_url)/api/ftp/list?path=$(ps5_url_encode "$dir")"
        return 0
    fi
    body="$(ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$dir")" none "" 2>/dev/null)" || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("entries", []):
    print("%s\t%d\t%s" % (entry.get("name", ""), int(entry.get("size", 0)),
                          "dir" if entry.get("is_dir") else "file"))
'
}

# ps5_ftp_files DIR [DRY]: ps5_ftp_list without the directories
# ("name<TAB>size", the format the size checks compare).
ps5_ftp_files() {
    ps5_ftp_list "$1" "${2:-0}" | awk -F'\t' '$3 == "file" { print $1 "\t" $2 }'
}

# ps5_ftp_has DIR NAME [DRY]: 0 when DIR lists NAME, a file or a directory.
ps5_ftp_has() {
    ps5_ftp_list "$1" "${3:-0}" | awk -F'\t' -v want="$2" '
$1 == want { found = 1 }
END { exit found ? 0 : 1 }'
}
