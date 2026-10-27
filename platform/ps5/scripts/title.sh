#!/usr/bin/env bash
# Install, launch, stop, inspect and write files for a homebrew title on the
# console.
#
#   platform/ps5/scripts/title.sh install build/ps5/title/PPSA99202
#   platform/ps5/scripts/title.sh launch  build/ps5/title/PPSA99202
#   platform/ps5/scripts/title.sh status  build/ps5/title/PPSA99202
#   platform/ps5/scripts/title.sh stop    build/ps5/title/PPSA99202
#   platform/ps5/scripts/title.sh install --dry-run build/ps5/title/PPSA99202
#   platform/ps5/scripts/title.sh put LOCAL_FILE <dataDir>/bench/bench.env
#
# What each verb does (docs/research/spike-b-memory.md sections 5 and 7):
#
#   install  copy the package folder to /data/homebrew/<TITLE_ID>/ with
#            ProsperoMgr's file API on 7070 (mkdir + raw upload, one request
#            per file), verify every size, then ask ShadowMountPlus (10101) for
#            an immediate scan. ShadowMountPlus copies sce_sys metadata to
#            /user/appmeta, nullfs-mounts the folder at /system_ex/app/<TID>,
#            writes /user/app/<TID>/mount.lnk and registers the title through
#            the SceShellCore TitleDir bridge; the tile then appears on the
#            home screen. Waits up to 90 s for GET /api/titles to list it.
#   launch   check the title is registered, check no other title is running,
#            then POST /api/titles/launch {"title_id":...} — ProsperoMgr's
#            sceUserServiceGetForegroundUser + sceSystemServiceLaunchApp. A
#            foreground user must be signed in. Waits for /api/titles/running
#            to report the id.
#   stop     write the quit request <dataDir>/title-quit (the app
#            polls it and ends itself with sceSystemServiceLoadExec("exit")) and
#            wait for /api/titles/running to go empty. If the title is still
#            running it says so and points at the console's own Close App —
#            killing a rendering title is the fragile path this project avoids.
#   status   read-only: ProsperoMgr, the title list, the running title, and the
#            package folder's files and sizes against the local copies.
#   put      upload one file to <dataDir>/... (the same file API) and
#            verify its size. Added for the bench title's selector, which is
#            written after the package is installed and is not part of it; it
#            takes no package and never writes outside the data directory.
#
# <dataDir> is the data folder of the identity the package was built as
# (identities.json): /data/ps5-splat-vr for the dev build, the release
# identity's own folder for a release package.
#
# Safety, the same rules as push-data.sh:
#   - every remote path is /data/homebrew/<our TITLE_ID> or that data folder;
#     a control character (tab, newline, DEL), "..", ".", an empty component
#     and a trailing slash are rejected before a request is built. Spaces and
#     other printable shell/URL metacharacters are allowed: the package ships
#     sample folders named "Living room" and "York street", the shell quotes
#     every path, and each request percent-encodes it (ps5_url_encode);
#   - no delete route of any kind is called, and nothing outside our two
#     directories is written or renamed;
#   - a title id that belongs to another project in the research (PS5LM,
#     PS5VR, PS5_Vulkan) is refused, because a collision reuses that title's
#     app.db entry and sandbox;
#   - agents never launch the release identity or touch its data folder
#     (AGENTS.md): install and status (read-only, /data/homebrew only) work for
#     it, launch/stop/put are refused before any request, and ps5-env.sh's
#     data-path guard refuses the private folder whatever verb is used;
#   - --dry-run prints, in order, exactly the requests a real run would send
#     and nothing is sent (PS5_HOST may be unset). It shares the real run's
#     request builders, so only the final send differs; a request that depends
#     on an answer the console would give (a missing directory, a title that is
#     not running, a poll) is printed with that condition next to it.
#
# Settings: PS5_HOST, PS5_PROSPERO_PORT (7070), PS5_SHADOWMOUNT_PORT (10101),
# PS5_TITLE_WAIT (90), PS5_TITLE_QUIT_WAIT (20), PS5_HTTP_TIMEOUT (300 for the
# eboot upload). Defaults for this project: the package is named after its
# title id and carries sce_sys/param.json + eboot.bin.
#
# Exit: 0 done, 1 a request failed, 2 usage/unsafe path/unusable package,
# 3 no ProsperoMgr (or, on install, no ShadowMountPlus) answer, 4 title not
# registered or launch refused, 5 the title is running (install refuses to
# overwrite a running eboot.bin) or was still running after the quit wait.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

usage() {
    # The header comment block: from line 2 to the first line that is not a
    # comment (the `set -euo pipefail` below), so the text never truncates when
    # the block grows.
    sed -n '2,${/^# SPDX-/q;/^[^#]/q;p;}' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# ---- arguments -------------------------------------------------------------
mode="run"
verb=""
package=""
remote_arg=""
for argument in "$@"; do
    case "$argument" in
        --dry-run) mode="dry-run" ;;
        -h|--help) usage; exit 0 ;;
        install|launch|stop|status|put)
            [ -z "$verb" ] || { usage >&2; exit 2; }
            verb="$argument" ;;
        -*)
            printf 'title: unknown option %s\n' "$argument" >&2
            usage >&2
            exit 2 ;;
        *)
            if [ -z "$package" ]; then
                package="$argument"
            elif [ -z "$remote_arg" ]; then
                remote_arg="$argument"
            else
                usage >&2
                exit 2
            fi ;;
    esac
done
[ -n "$verb" ] || { usage >&2; exit 2; }
if [ "$verb" = "put" ]; then
    # put SOURCE REMOTE: no package, no title id.
    [ -n "$remote_arg" ] || { usage >&2; exit 2; }
else
    [ -n "$package" ] && [ -z "$remote_arg" ] || { usage >&2; exit 2; }
fi

ps5_env_load

say() { printf 'title: %s\n' "$*" >&2; }
fail() { say "$1"; exit "${2:-1}"; }
unsafe() { say "unsafe path: $*"; say "nothing was sent"; exit 2; }

prospero_port="${PS5_PROSPERO_PORT:-7070}"
shadowmount_port="${PS5_SHADOWMOUNT_PORT:-10101}"
wait_seconds="${PS5_TITLE_WAIT:-90}"
quit_wait="${PS5_TITLE_QUIT_WAIT:-20}"
# The eboot is tens of MiB; the default 30 s from ps5-env.sh is for JSON.
export PS5_HTTP_TIMEOUT="${PS5_HTTP_TIMEOUT:-300}"

# ---- the package ------------------------------------------------------------
# A package is a folder named after its title id with the two files
# ShadowMountPlus verifies before it registers anything. `put` has no package:
# it writes one file under the data directory.
title_id=""
if [ "$verb" != "put" ]; then
    [ -d "$package" ] || fail "no such directory: $package" 2
    [ -f "$package/eboot.bin" ] || fail "$package has no eboot.bin (make -C platform/ps5/title)" 2
    [ -f "$package/sce_sys/param.json" ] || fail "$package has no sce_sys/param.json" 2

    title_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
        "$package/sce_sys/param.json" 2>/dev/null)" || fail "$package/sce_sys/param.json is not JSON with a titleId" 2
    case "$title_id" in
        PPSA[0-9][0-9][0-9][0-9][0-9]) ;;
        *) fail "title id '$title_id' is not PPSA##### — a homebrew title id has four letters and five digits" 2 ;;
    esac
    # A collision is not a naming question: the title would reuse that project's
    # app.db entry and sandbox (docs/research/spike-b-memory.md section 5).
    case "$title_id" in
        PPSA99575|PPSA99581|PPSA99177|PPSA99176|PPSA99014|PPSA99015|PPSA99170|PPSA99988)
            fail "$title_id belongs to another project in the research (PS5LM, PS5VR or PS5_Vulkan); pick our own id" 2 ;;
    esac
    [ "$(basename -- "$package")" = "$title_id" ] ||
        fail "the package folder is $(basename -- "$package") but param.json says $title_id; they must match" 2
    remote_root="/data/homebrew/$title_id"
else
    remote_root=""
fi

# The data folder of the identity this command acts on: the package's id for
# install/launch/stop/status, the selected identity for put. One source: the
# table (ps5_title_data_dir / identity.py), never a path written here.
if [ "$verb" = "put" ]; then
    data_root="$(ps5_title_data_dir)"
else
    data_root="$(ps5_title_field dataDir --title-id "$title_id")" ||
        fail "cannot read the identity of $title_id (platform/ps5/title/identities.json)" 2
fi
[ -n "$data_root" ] || fail "no data folder for ${title_id:-the selected identity} in platform/ps5/title/identities.json" 2

# Only the dev identity may be launched or stopped, and only its data folder may
# be written: agents never drive the release install (AGENTS.md). install and
# status stay allowed for a release package: they touch /data/homebrew only.
case "$verb" in
    launch|stop|put) ps5_agent_guard_identity "${title_id:-}" ;;
esac

# ---- remote-path policy -----------------------------------------------------
# Only /data/homebrew/<our title id> and below, plus the identity's data folder
# for the quit request and `put`. Checked before any request is built; the
# private release folder is refused by ps5_guard_remote_path first.
validate_remote_path() {
    local path="$1" mode="$2" rest part
    ps5_guard_remote_path "$path"
    case "$mode" in
        homebrew)
            case "$path" in
                "/data/homebrew/$title_id"|"/data/homebrew/$title_id/"*) ;;
                *) unsafe "remote path '$path' is outside /data/homebrew/$title_id" ;;
            esac ;;
        data)
            case "$path" in
                "$data_root"|"$data_root"/*) ;;
                *) unsafe "remote path '$path' is outside $data_root" ;;
            esac ;;
    esac
    case "$path" in
        *[[:cntrl:]]*) unsafe "remote path '$path' has a control character" ;;
        */) unsafe "remote path '$path' ends with a slash" ;;
    esac
    # Split on "/" without word splitting or globbing, so a component that is
    # "*", "?" or "[" is checked as itself (a space is not split at all).
    rest="${path#/}"
    while [ -n "$rest" ]; do
        part="${rest%%/*}"
        case "$part" in
            ''|.|..) unsafe "remote path '$path' has a '$part' component" ;;
        esac
        case "$rest" in
            */*) rest="${rest#*/}" ;;
            *) rest="" ;;
        esac
    done
    return 0
}

remote_root="/data/homebrew/$title_id"
if [ "$verb" != "put" ]; then
    remote_root="/data/homebrew/$title_id"
    validate_remote_path "$remote_root" homebrew
fi
# The quit file belongs to launch/stop only. install and status never name the
# data folder, so they stay usable for a release package (its folder is private).
quit_file=""
case "$verb" in
    launch|stop)
        quit_file="$data_root/title-quit"
        validate_remote_path "$quit_file" data ;;
esac

# ---- local file list --------------------------------------------------------
# "relative-path<TAB>bytes<TAB>relative-directory", sorted: parents first,
# deterministic order, and the directory is "" for a file at the root (so
# nothing has to resolve "dirname ." into a path the policy would reject).
list_files() {
    python3 - "$package" <<'PY'
import os, sys
root = sys.argv[1]
found = []
for directory, _, files in os.walk(root):
    for name in files:
        full = os.path.join(directory, name)
        relative = os.path.relpath(full, root).replace(os.sep, "/")
        parent = os.path.dirname(relative)
        found.append((relative, os.path.getsize(full), parent))
for relative, size, parent in sorted(found):
    print("%s\t%d\t%s" % (relative, size, parent))
PY
}

list_dirs() {
    python3 - "$package" <<'PY'
import os, sys
root = sys.argv[1]
found = [""]
for directory, subdirs, _ in os.walk(root):
    for name in subdirs:
        found.append(os.path.relpath(os.path.join(directory, name), root).replace(os.sep, "/"))
for relative in sorted(found, key=lambda r: (r.count("/"), r)):
    print(relative)
PY
}

# Every ?path= value goes through ps5_url_encode and every ftp JSON body through
# ps5_json_path (both from ps5-env.sh): one encoder for the whole repo, so a
# folder name with a space, "#", "&", "+" or "%" is sent as one well-formed
# request target.
json_title() { python3 -c 'import json,sys;print(json.dumps({"title_id":sys.argv[1]}))' "$1"; }
base_url() { printf 'http://%s:%s' "${PS5_HOST:-<PS5_HOST>}" "$prospero_port"; }

human() {
    python3 -c 'import sys; n=int(sys.argv[1]); u=["B","KiB","MiB","GiB"]; i=0
n=float(n)
while n>=1024 and i<3: n/=1024; i+=1
print(f"{n:.2f} {u[i]}")' "$1"
}

# ---- requests: one builder, one send ----------------------------------------
# Every request this script sends is built here or in upload_file(), and
# --dry-run only swaps the send, so its output is the real run's wire traffic:
#
#   http METHOD PATH KIND [DATA] [NOTE]
#
# PATH already carries its query string (ps5_url_encode/ps5_json_path). KIND is
# "none" or "json". The answer is read back through "$(http ...)", so in a dry
# run the line goes to fd 9 — the caller's own stdout, untouched by the command
# substitution — and the answer is empty. NOTE says what a real run would decide
# from an answer the dry run cannot have (a directory that may be missing, a
# title that may not be running, a poll).
exec 9>&1
http() {
    local method="$1" path="$2" kind="$3" data="${4:-}" note="${5:-}" line
    # A dry run must refuse exactly what a live run refuses (the private data
    # folder), and the live path is guarded again inside ps5_http.
    ps5_guard_http_path "$path" "$data"
    if [ "$mode" = "dry-run" ]; then
        line="  $method $(base_url)$path"
        case "$kind" in
            json) line="$line  body $data" ;;
        esac
        [ -z "$note" ] || line="$line   ($note)"
        printf '%s\n' "$line" >&9
        return 0
    fi
    ps5_http "$method" "$path" "$kind" "$data"
}

# ---- streaming upload -------------------------------------------------------
# One POST per file: /api/ftp/upload?path=...&overwrite=1 with the file as the
# whole body, streamed in 1 MiB writes so nothing is held in memory. The URL is
# built once by upload_target() and used by the dry-run line and the request.
upload_target() { printf '/api/ftp/upload?path=%s&overwrite=1' "$(ps5_url_encode "$1")"; }

# upload_file LOCAL REMOTE POLICY [NOTE]  (POLICY is homebrew or data, the same
# check every other request goes through)
upload_file() {
    local local_file="$1" remote_path="$2" policy="$3" note="${4:-}" url bytes line
    validate_remote_path "$remote_path" "$policy"
    url="$(upload_target "$remote_path")"
    bytes="$(wc -c <"$local_file" | tr -d ' ')"
    if [ "$mode" = "dry-run" ]; then
        line="  POST $(base_url)$url  body $bytes bytes ($(basename -- "$remote_path"))"
        [ -z "$note" ] || line="$line   ($note)"
        printf '%s\n' "$line" >&9
        return 0
    fi
    TITLE_LOCAL="$local_file" TITLE_URL="$url" TITLE_REMOTE="$remote_path" \
    TITLE_HOST="$PS5_HOST" TITLE_PORT="$prospero_port" python3 - <<'PY'
import http.client, os, sys

local = os.environ["TITLE_LOCAL"]
path = os.environ["TITLE_URL"]
remote = os.environ["TITLE_REMOTE"]
host = os.environ["TITLE_HOST"]
port = int(os.environ["TITLE_PORT"])
total = os.path.getsize(local)

def say(message):
    sys.stderr.write("title: %s\n" % message)
    sys.stderr.flush()

conn = http.client.HTTPConnection(host, port, timeout=300)
status = 0
body = b""
try:
    try:
        conn.putrequest("POST", path)
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(total))
        conn.endheaders()
        sent = 0
        with open(local, "rb") as source:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                if total > 16 * 1024 * 1024 and sent % (8 * 1024 * 1024) < 1024 * 1024:
                    say("%s: %.0f%% (%d of %d bytes)"
                        % (remote, 100.0 * sent / total, sent, total))
        response = conn.getresponse()
        body = response.read()
        status = response.status
    except OSError as error:
        # One line, no traceback: refused, timed out, or the server closed early.
        sys.stderr.write("title: upload %s: %s: %s\n" % (remote, type(error).__name__, error))
        sys.exit(1)
finally:
    conn.close()

if status >= 400:
    sys.stderr.write("title: upload %s: HTTP %d %s\n"
                     % (remote, status, body[:200].decode("utf-8", "replace")))
    sys.exit(1)
say("%s: %s uploaded" % (remote, total))
PY
}

# ---- ProsperoMgr helpers ----------------------------------------------------
prospero_status() {
    local body
    body=$(http GET /api/status none "" "ProsperoMgr must answer, the same service run.sh uses") || {
        say "no HTTP answer from ProsperoMgr on ${PS5_HOST:-<PS5_HOST>}:$prospero_port"
        say "launch ProsperoMgr on the console (the same service run.sh uses), then retry"
        exit 3
    }
    [ "$mode" = "dry-run" ] && return 0
    [ "$(printf '%s' "$body" | ps5_json_field name)" = "Prospero Manager" ] || {
        say "the service on $prospero_port is not ProsperoMgr; refusing to continue"
        exit 3
    }
    say "ProsperoMgr $(printf '%s' "$body" | ps5_json_field version) on ${PS5_HOST:-<PS5_HOST>}:$prospero_port"
}

# Remote directory listing -> "name<TAB>size" lines, empty on error.
remote_list() {  # remote_list DIR [NOTE]
    local body
    body=$(http GET "/api/ftp/list?path=$(ps5_url_encode "$1")" none "" \
                "${2:-files and sizes of that folder}" 2>/dev/null) || return 0
    [ "$mode" = "dry-run" ] && return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("entries", []):
    if not entry.get("is_dir"):
        print("%s\t%d" % (entry.get("name", ""), int(entry.get("size", 0))))
'
}

remote_size() {  # remote_size DIR NAME [NOTE] -> bytes, empty when absent
    remote_list "$1" "${3:-}" | awk -F'\t' -v want="$2" '$1 == want {print $2; exit}'
}

remote_titles() {  # remote_titles [NOTE] -> one title id per line
    local body
    body=$(http GET /api/titles none "" "${1:-the title list}" 2>/dev/null) || return 0
    [ "$mode" = "dry-run" ] && return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("titles", []):
    print(entry.get("title_id", ""))
' || true
}

running_title() {  # running_title [NOTE] -> the running title id, empty when none
    local body
    body=$(http GET /api/titles/running none "" "${1:-which title runs}" 2>/dev/null) || return 0
    [ "$mode" = "dry-run" ] && return 0
    printf '%s' "$body" | ps5_json_field title_id || true
}

# Create a remote directory if its listing fails: the server's mkdir is not
# recursive, so callers walk parents before children. In a dry run this prints
# the fresh-install pair — the listing, then the mkdir a failed listing sends.
ensure_remote_dir() {
    local target="$1"
    if [ "$mode" = "dry-run" ]; then
        http GET "/api/ftp/list?path=$(ps5_url_encode "$target")" none "" "already there?"
        http POST /api/ftp/mkdir json "$(ps5_json_path "$target")" "only when that listing fails"
        return 0
    fi
    if http GET "/api/ftp/list?path=$(ps5_url_encode "$target")" none "" >/dev/null 2>&1; then
        return 0
    fi
    say "mkdir $target"
    http POST /api/ftp/mkdir json "$(ps5_json_path "$target")" >/dev/null ||
        fail "cannot create $target" 1
}

# ShadowMountPlus scans on demand; a TCP probe decides whether to send it. The
# probe is not an HTTP request, so the dry run prints the scan with its
# condition instead.
shadowmount_scan() {
    local body='{"reset_attempts":false}'
    if [ "$mode" = "dry-run" ]; then
        printf '  POST http://%s:%s/api/v1/scan  body %s   (only when %s is open)\n' \
               "${PS5_HOST:-<PS5_HOST>}" "$shadowmount_port" "$body" "$shadowmount_port" >&9
        return 0
    fi
    say "shadowmount: POST /api/v1/scan on ${PS5_HOST}:$shadowmount_port"
    if ps5_port_open "$PS5_HOST" "$shadowmount_port" 3; then
        printf '%s' "$body" | PS5_HTTP_HOST="$PS5_HOST" PS5_HTTP_PORT="$shadowmount_port" \
        PS5_HTTP_TIMEOUT=15 python3 -c '
import http.client, os, sys
conn = http.client.HTTPConnection(os.environ["PS5_HTTP_HOST"], int(os.environ["PS5_HTTP_PORT"]), timeout=15)
try:
    conn.request("POST", "/api/v1/scan", body=sys.stdin.read(),
                 headers={"Content-Type": "application/json"})
    response = conn.getresponse()
    sys.stderr.write("title: shadowmount scan: HTTP %d\n" % response.status)
    sys.stderr.flush()
finally:
    conn.close()
'
        return 0
    fi
    say "ShadowMountPlus is not answering on port $shadowmount_port"
    say "start it on the console (kstuff, HEN and ShadowMountPlus after a boot), then run:"
    say "  platform/ps5/scripts/title.sh install $package"
    exit 3
}

# Wait for the title list to contain ours: ShadowMountPlus scans on its own
# schedule, the forced scan only makes that immediate.
wait_registered() {
    local waited=0 body
    while [ "$waited" -lt "$wait_seconds" ]; do
        body="$(remote_titles || true)"
        if printf '%s\n' "$body" | grep -qx "$title_id"; then
            return 0
        fi
        sleep 5
        waited=$((waited + 5))
    done
    return 1
}

wait_running() {  # wait_running WANT(1 running, 0 stopped) SECONDS
    local want="$1" limit="$2" waited=0 current
    while [ "$waited" -lt "$limit" ]; do
        current="$(running_title || true)"
        if [ "$want" = "1" ] && [ "$current" = "$title_id" ]; then return 0; fi
        if [ "$want" = "0" ] && [ "$current" != "$title_id" ]; then return 0; fi
        sleep 2
        waited=$((waited + 2))
    done
    return 1
}

# ---- verbs ------------------------------------------------------------------
# Each verb calls the same helpers and request builders in both modes; a
# dry-run only stops where a real run would wait for an answer (the quit wait,
# the registration poll), and prints that request with its condition instead.
install_title() {
    say "install $title_id -> $remote_root"
    prospero_status
    [ -n "${PS5_HOST:-}" ] || [ "$mode" = "dry-run" ] ||
        fail "set PS5_HOST or add PS5_HOST=... to .env.local" 2

    # Never overwrite a running eboot.bin (the dev-loop rule in the research).
    current="$(running_title "the check: if this is $title_id, exit 5 and upload nothing")"
    if [ "$mode" != "dry-run" ] && [ "$current" = "$title_id" ]; then
        say "$title_id is running; stop it first: title.sh stop $package"
        exit 5
    fi

    # /data/homebrew must exist; its parent is the console's, so only the
    # documented homebrew root is ever created. Parents before children: the
    # server's mkdir is not recursive.
    ensure_remote_dir /data/homebrew
    while IFS= read -r relative_dir; do
        [ -n "$relative_dir" ] || continue
        target="$remote_root/$relative_dir"
        validate_remote_path "$target" homebrew
        ensure_remote_dir "$target"
    done <<EOF
$(list_dirs)
EOF

    while IFS="$(printf '\t')" read -r relative bytes relative_dir; do
        say "upload $relative ($(human "$bytes"))"
        upload_file "$package/$relative" "$remote_root/$relative" homebrew ||
            fail "upload of $relative failed" 1
    done <<EOF
$(list_files)
EOF

    # Verify what is on the console, not what the replies said. The same loop
    # runs in a dry run, where remote_size() prints its listing and returns
    # nothing, so every verification request appears in order too.
    mismatch=0
    while IFS="$(printf '\t')" read -r relative bytes relative_dir; do
        dir="$remote_root"
        [ -z "$relative_dir" ] || dir="$remote_root/$relative_dir"
        name="$(basename -- "$relative")"
        actual="$(remote_size "$dir" "$name" "verify $name = $bytes bytes")" || true
        [ "$mode" = "dry-run" ] && continue
        if [ "$actual" != "$bytes" ]; then
            say "size mismatch: $relative local $bytes, console ${actual:-missing}"
            mismatch=1
        fi
    done <<EOF
$(list_files)
EOF
    if [ "$mismatch" -ne 0 ]; then
        fail "the console copy does not match the package; re-run install" 1
    fi
    if [ "$mode" = "dry-run" ]; then
        # Register (and mount) through ShadowMountPlus, then poll the title list.
        shadowmount_scan
        http GET /api/titles none "" \
             "poll every 5 s, up to ${wait_seconds}s, until $title_id is listed"
        return 0
    fi
    say "files verified: $(list_files | wc -l | tr -d ' ') file(s) under $remote_root"

    shadowmount_scan
    say "waiting up to ${wait_seconds}s for $title_id in GET /api/titles"
    if wait_registered; then
        say "registered: $title_id is in the title list (tile on the home screen)"
        say "login-required? a foreground user must be signed in for launch"
        say "next: platform/ps5/scripts/title.sh launch $package"
        return 0
    fi
    say "not registered after ${wait_seconds}s."
    say "check the ShadowMountPlus log (its debug log via /api/v1/debug-log) for"
    say "\"Register failed: TitleDir bridge unavailable\" — that means its ShellCore"
    say "hook is stale and the payload must be reloaded."
    exit 4
}

launch_title() {
    say "launch $title_id"
    prospero_status
    list="$(remote_titles "must contain $title_id: exit 4 when it does not")"
    if [ "$mode" != "dry-run" ] && ! printf '%s\n' "$list" | grep -qx "$title_id"; then
        say "$title_id is not registered yet (not in GET /api/titles)"
        say "run: platform/ps5/scripts/title.sh install $package"
        exit 4
    fi
    current="$(running_title "if another title is running: exit 5, nothing is launched")"
    if [ "$mode" != "dry-run" ] && [ -n "$current" ] && [ "$current" != "$title_id" ]; then
        say "another title is running ($current); the system launches one app at a time"
        say "close it first: title.sh stop $package  (or Close App on the console)"
        exit 5
    fi
    body=$(http POST /api/titles/launch json "$(json_title "$title_id")") || {
        say "launch refused; a foreground user must be signed in and the title registered"
        exit 4
    }
    if [ "$mode" = "dry-run" ]; then
        http GET /api/titles/running none "" "poll every 2 s, up to 30s, for $title_id"
        return 0
    fi
    printf '%s\n' "$body"
    [ "$(printf '%s' "$body" | ps5_json_field ok)" = "true" ] || exit 4
    say "waiting up to 30s for /api/titles/running to report $title_id"
    if wait_running 1 30; then
        say "running: $title_id"
        say "read its log: platform/ps5/scripts/klog.sh"
        return 0
    fi
    say "the launch was accepted but $title_id is not reported running yet"
    say "check klog for launchApp($title_id) and EXEC /app0/eboot.bin"
    exit 4
}

stop_title() {
    say "stop $title_id"
    prospero_status
    current="$(running_title "the check: if this is not $title_id, exit 0 and send no quit request")"
    if [ "$mode" != "dry-run" ] && [ "$current" != "$title_id" ]; then
        say "not running (running title: ${current:-none}); nothing to stop"
        return 0
    fi
    # The graceful stop: our own file, the app polls it and ends itself through
    # sceSystemServiceLoadExec("exit"). Never a kill: the research records that
    # killing a rendering title has been followed by lost consoles.
    quit_local="$(mktemp "${TMPDIR:-/tmp}/ps5splat-quit.XXXXXX")"
    printf 'quit\n' >"$quit_local"
    upload_file "$quit_local" "$quit_file" data "5 bytes of \"quit\": the title polls it and exits itself" ||
        { rm -f "$quit_local"; fail "could not write $quit_file" 1; }
    if [ "$mode" = "dry-run" ]; then
        rm -f "$quit_local"
        http GET /api/titles/running none "" \
             "poll every 2 s, up to ${quit_wait}s, for an empty title_id"
        return 0
    fi
    rm -f "$quit_local"
    say "quit requested; waiting up to ${quit_wait}s"
    if wait_running 0 "$quit_wait"; then
        say "stopped: $title_id is no longer running"
        return 0
    fi
    say "$title_id is still running after ${quit_wait}s"
    say "close it on the console: press PS, highlight the app, Options -> Close App."
    say "A forced kill from the API is not used here: see docs/research/spike-b-memory.md section 9."
    exit 5
}

status_title() {
    say "status $title_id ($package)"
    prospero_status
    list="$(remote_titles "is $title_id registered?")"
    current="$(running_title "the running title")"
    if [ "$mode" != "dry-run" ]; then
        if printf '%s\n' "$list" | grep -qx "$title_id"; then
            say "registered: yes"
        else
            say "registered: no (title.sh install $package)"
        fi
        say "running: ${current:-none}"
    fi

    # One listing per file, exactly what a real run sends; the loop is shared,
    # so a dry run prints the same set and stops before comparing.
    missing=0
    while IFS="$(printf '\t')" read -r relative bytes relative_dir; do
        dir="$remote_root"
        [ -z "$relative_dir" ] || dir="$remote_root/$relative_dir"
        name="$(basename -- "$relative")"
        actual="$(remote_size "$dir" "$name" "verify $name = $bytes bytes")" || true
        [ "$mode" = "dry-run" ] && continue
        if [ "$actual" = "$bytes" ]; then
            printf '  OK   %-30s %10s\n' "$relative" "$(human "$bytes")"
        else
            printf '  DIFF %-30s local %s, console %s\n' "$relative" "$(human "$bytes")" \
                   "$( [ -n "$actual" ] && human "$actual" || echo missing )"
            missing=1
        fi
    done <<EOF
$(list_files)
EOF
    [ "$mode" = "dry-run" ] && return 0
    if [ "$missing" -ne 0 ]; then
        say "the console copy differs from the package; run: title.sh install $package"
        exit 1
    fi
    say "files verified under $remote_root"
    if ! printf '%s\n' "$list" | grep -qx "$title_id"; then
        exit 4
    fi
    return 0
}

# ---- put: one file under /data/ps5-splat-vr --------------------------------
# Added for the bench title's selector (run-bench.sh --title). The destination
# is a file path; its parent must already exist (push-data.sh creates
# /data/ps5-splat-vr/bench). Same upload route and size check as install.
put_file() {
    local source="$package" remote="$remote_arg" bytes dir name actual
    say "put $source -> $remote"
    [ -f "$source" ] || fail "no such file: $source" 2
    validate_remote_path "$remote" data
    bytes="$(wc -c <"$source" | tr -d ' ')"
    dir="$(dirname -- "$remote")"
    name="$(basename -- "$remote")"
    prospero_status
    [ -n "${PS5_HOST:-}" ] || [ "$mode" = "dry-run" ] ||
        fail "set PS5_HOST or add PS5_HOST=... to .env.local" 2
    upload_file "$source" "$remote" data ||
        fail "upload of $remote failed (does its directory exist? push-data.sh creates /data/ps5-splat-vr/bench)" 1
    actual="$(remote_size "$dir" "$name" "verify $name = $bytes bytes")" || true
    [ "$mode" = "dry-run" ] && return 0
    [ "$actual" = "$bytes" ] || fail "$remote is ${actual:-missing} bytes on the console, local $bytes" 1
    say "put $remote ($(human "$bytes"), size verified)"
}

case "$verb" in
    install) install_title ;;
    launch) launch_title ;;
    stop) stop_title ;;
    status) status_title ;;
    put) put_file ;;
    *) usage >&2; exit 2 ;;
esac
