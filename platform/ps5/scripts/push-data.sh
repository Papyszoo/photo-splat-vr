#!/usr/bin/env bash
# Push data to the console: the Spike A GGUFs and bench photo into
# <dataDir>/{models,bench}/, and/or scene folders into <dataDir>/scenes/ for the
# viewer title (platform/ps5/viewer), where <dataDir> is the dev identity's data
# folder from platform/ps5/title/identities.json (/data/ps5-splat-vr). Agents
# never push into the release install's private folder (ps5-env.sh guards).
#
#   platform/ps5/scripts/push-data.sh --check      # plan only, no console I/O
#   platform/ps5/scripts/push-data.sh --dry-run    # every HTTP request, no I/O
#   platform/ps5/scripts/push-data.sh              # upload what is missing
#   platform/ps5/scripts/push-data.sh --verify     # compare remote sizes only
#   PUSH_PHOTO=/path/to/photo.jpg platform/ps5/scripts/push-data.sh
#
# Scene folders (the viewer title reads <dataDir>/scenes/):
#
#   platform/ps5/scripts/push-data.sh --scene  "$SPLAT_DATA_DIR/runs/sharp_e2e/photo03_reading_room"
#   platform/ps5/scripts/push-data.sh --scenes "$SPLAT_DATA_DIR/runs/render/comfort/scenes"
#
# --scene pushes one folder (its scene.psplat, and meta.json when present);
# --scenes pushes every immediate subfolder that holds scene.psplat. Either
# flag may be repeated, and giving either one plans ONLY the scenes (no GGUF or
# photo), so a scene can be pushed without the Spike A weights being present.
# Each lands under <dataDir>/scenes/<folder-name>/.
#
# Route: ProsperoMgr's native file-manager API on $PS5_PROSPERO_PORT (7070),
# which is already running for deploy.sh/klog.sh. It is the only service this
# script talks to, over plain HTTP, and it never sends anything to the elfldr,
# klog or ftpsrv ports. `POST /api/ftp/upload/chunk` appends each chunk straight
# into a checkpoint file next to the destination and renames it when complete,
# so a 1.25 GiB transfer is resumable and never exposes a partial GGUF.
#
# Safety:
#   - every remote path must be the dev identity's data folder or below; "..",
#     "." and empty components are rejected before any request is built, and the
#     release identity's private folder is refused outright;
#   - a file is only replaced after its complete upload has arrived, by the
#     server's own rename (this script has no delete call of any kind);
#   - an interrupted upload resumes from the size of the server-side
#     checkpoint (`<name>.pmgr-part-<id>`, id derived from path and size), and
#     a completed upload is size-verified before it is reported;
#   - local and remote paths are quoted everywhere, so spaces (and other
#     shell metacharacters) are carried through unchanged.
#
# Settings: PS5_HOST, PS5_PROSPERO_PORT (7070), PUSH_CHUNK_MB (64),
# PUSH_PHOTO (default: golden/inputs/photo01_living_room.jpg), SPLAT_DATA_DIR,
# TITLE_ID (identity selection, dev by default).
#
# Exit: 0 everything present/uploaded, 1 an upload failed, 2 usage/missing local
# file or an unsafe path, 3 no ProsperoMgr or the route is missing.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

mode="push"
scene_dirs=()
scenes_dirs=()
usage() { sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --push|'') ;;
        --check) mode="check" ;;
        --dry-run) mode="dry-run" ;;
        --verify) mode="verify" ;;
        --scene)
            shift
            [ "$#" -gt 0 ] || ps5_die "push-data: --scene needs a directory"
            scene_dirs+=("$1") ;;
        --scene=*) scene_dirs+=("${1#--scene=}") ;;
        --scenes)
            shift
            [ "$#" -gt 0 ] || ps5_die "push-data: --scenes needs a directory"
            scenes_dirs+=("$1") ;;
        --scenes=*) scenes_dirs+=("${1#--scenes=}") ;;
        -h|--help) usage; exit 0 ;;
        *) ps5_die "push-data: unknown argument '$1' (--check, --dry-run, --verify, --scene DIR, --scenes DIR)" ;;
    esac
    shift
done

ps5_env_load
data="$(ps5_data_dir)"
# Agents push data only for the dev install (AGENTS.md): the release identity is
# refused here, before a path is validated or a request is printed.
ps5_agent_guard_identity

say() { printf 'push-data: %s\n' "$*" >&2; }
unsafe() { say "unsafe path: $*"; say "nothing was sent"; exit 2; }

# ---- remote-path policy ----------------------------------------------------
# Only the selected identity's data folder and its descendants. Lexical check on
# the path as written: no empty, "." or ".." component, no trailing slash except
# the root, and no control characters. Everything the script later sends is
# built from a validated directory plus a single validated file name.
data_root="$(ps5_title_data_dir)"
REMOTE_ROOT="${PUSH_REMOTE_ROOT:-$data_root}"
# One trailing slash is a typo, not a path escape; strip it before validating.
case "$REMOTE_ROOT" in
    */) REMOTE_ROOT="${REMOTE_ROOT%/}" ;;
esac

validate_remote_dir() {
    local path="$1" rest part
    ps5_guard_remote_path "$path"
    case "$path" in
        "$data_root") return 0 ;;
        "$data_root"/*) ;;
        *) unsafe "remote directory '$path' is outside $data_root" ;;
    esac
    case "$path" in
        */) unsafe "remote directory '$path' has a trailing slash" ;;
        *$'\n'*|*$'\t'*) unsafe "remote directory '$path' has a control character" ;;
    esac
    rest="${path#/}"
    local IFS='/'
    for part in $rest; do
        case "$part" in
            ''|.|..) unsafe "remote directory '$path' has a '$part' component" ;;
        esac
    done
    return 0
}

validate_remote_name() {
    local name="$1"
    case "$name" in
        ''|.|..|*/*|*$'\n'*|*$'\t'*) unsafe "remote file name '$name'" ;;
    esac
}

validate_remote_dir "$REMOTE_ROOT"
REMOTE_MODELS="$REMOTE_ROOT/models"
REMOTE_BENCH="$REMOTE_ROOT/bench"
REMOTE_SCENES="$REMOTE_ROOT/scenes"
validate_remote_dir "$REMOTE_MODELS"
validate_remote_dir "$REMOTE_BENCH"
validate_remote_dir "$REMOTE_SCENES"

# ---- scenes (the viewer title's input) --------------------------------------
# A scene folder is what core/scene reads: scene.psplat, and meta.json for the
# capture intrinsics the renderer projects with. The viewer finds them by
# scanning the immediate subfolders of <dataDir>/scenes, so the name
# has to survive as a folder name and as SCENE=<name> in viewer.env: letters,
# digits, dot, dash and underscore only.
scene_names=()
scene_sources=()
add_scene() {
    local dir="$1" name
    [ -d "$dir" ] || ps5_die "push-data: no such scene folder: $dir"
    [ -f "$dir/scene.psplat" ] || ps5_die "push-data: $dir has no scene.psplat"
    name="$(basename -- "$dir")"
    case "$name" in
        ''|.|..|*[!A-Za-z0-9._-]*) ps5_die "push-data: scene folder name '$name' is not [A-Za-z0-9._-]+, so it could not be named in viewer.env" ;;
    esac
    scene_names+=("$name")
    scene_sources+=("$dir")
}

for dir in "${scene_dirs[@]+"${scene_dirs[@]}"}"; do
    add_scene "$dir"
done
for parent in "${scenes_dirs[@]+"${scenes_dirs[@]}"}"; do
    [ -d "$parent" ] || ps5_die "push-data: no such directory: $parent"
    found=0
    for candidate in "$parent"/*/; do
        [ -f "${candidate}scene.psplat" ] || continue
        add_scene "${candidate%/}"
        found=$((found + 1))
    done
    [ "$found" -gt 0 ] || ps5_die "push-data: no subfolder of $parent holds scene.psplat"
done
scenes_only=0
if [ "${#scene_names[@]}" -gt 0 ]; then
    scenes_only=1
fi

scene_dest() { printf '%s/%s' "$REMOTE_SCENES" "$1"; }

# ---- local files -----------------------------------------------------------
SHARP_GGUF="${PUSH_SHARP_GGUF:-$data/weights/sharp_2572gikvuh-f16-v1.gguf}"
DA2_GGUF="${PUSH_DA2_GGUF:-$data/weights/dav2s-f16.gguf}"
PHOTO="${PUSH_PHOTO:-$data/golden/inputs/photo01_living_room.jpg}"
chunk_mb="${PUSH_CHUNK_MB:-64}"

case "$chunk_mb" in
    ''|*[!0-9]*) ps5_die "push-data: PUSH_CHUNK_MB must be a whole number of MiB, got '$chunk_mb'" ;;
esac
[ "$chunk_mb" -ge 1 ] && [ "$chunk_mb" -le 255 ] ||
    ps5_die "push-data: PUSH_CHUNK_MB must be 1..255 (ProsperoMgr caps a request at 264 MiB)"

# Parallel arrays: a name, its local path and its validated remote directory.
# (No '|'-joined records, so a local path with any separator is safe.)
plan_name=()
plan_src=()
plan_dir=()
add_plan() {
    validate_remote_name "$1"
    validate_remote_dir "$3"
    plan_name+=("$1")
    plan_src+=("$2")
    plan_dir+=("$3")
}
if [ "$scenes_only" -eq 0 ]; then
    add_plan "sharp_2572gikvuh-f16-v1.gguf" "$SHARP_GGUF" "$REMOTE_MODELS"
    add_plan "dav2s-f16.gguf" "$DA2_GGUF" "$REMOTE_MODELS"
    add_plan "photo.jpg" "$PHOTO" "$REMOTE_BENCH"
else
    # Scenes only: the GGUF plan is not part of this push.
    say "scenes only: ${#scene_names[@]} folder(s), the GGUF/photo plan is skipped"
fi
for i in "${!scene_names[@]}"; do
    add_plan "scene.psplat" "${scene_sources[$i]}/scene.psplat" "$(scene_dest "${scene_names[$i]}")"
    if [ -f "${scene_sources[$i]}/meta.json" ]; then
        add_plan "meta.json" "${scene_sources[$i]}/meta.json" "$(scene_dest "${scene_names[$i]}")"
    fi
done

file_size() {
    if [ -f "$1" ]; then stat -f%z "$1" 2>/dev/null || stat -c%s "$1"; else echo 0; fi
}

human() { python3 -c 'import sys; n=int(sys.argv[1]); u=["B","KiB","MiB","GiB"]; i=0
n=float(n)
while n>=1024 and i<3: n/=1024; i+=1
print(f"{n:.2f} {u[i]}")' "$1"; }

missing=0
for i in "${!plan_src[@]}"; do
    src="${plan_src[$i]}"
    if [ ! -f "$src" ]; then
        say "missing local file: $src"
        missing=1
    elif [ ! -r "$src" ]; then
        say "unreadable local file: $src"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || { say "nothing was sent"; exit 2; }

if [ "$mode" = "check" ]; then
    say "plan (no console I/O):"
    total=0
    for i in "${!plan_name[@]}"; do
        name="${plan_name[$i]}"
        src="${plan_src[$i]}"
        dir="${plan_dir[$i]}"
        bytes="$(file_size "$src")"
        total=$((total + bytes))
        printf '  %-34s %10s  %s -> %s/%s\n' "$name" "$(human "$bytes")" "$src" "$dir" "$name"
    done
    printf '  total %s in %d file(s), %d MiB chunks\n' "$(human "$total")" "${#plan_name[@]}" "$chunk_mb"
    if [ "$scenes_only" -eq 0 ]; then
        say "bench.env is written by push-data.sh at $REMOTE_BENCH/bench.env"
    else
        say "scenes land under $REMOTE_SCENES/<folder>/; the viewer reads them from there"
    fi
    exit 0
fi

# ---- ProsperoMgr -----------------------------------------------------------
[ -n "${PS5_HOST:-}" ] || ps5_die "push-data: set PS5_HOST or add PS5_HOST=... to .env.local"
port="${PS5_PROSPERO_PORT:-7070}"
case "$port" in
    ''|*[!0-9]*) ps5_die "push-data: PS5_PROSPERO_PORT must be a whole number, got '$port'" ;;
esac
case "$PS5_HOST" in
    */*|*$'\n'*|*$'\t'*) ps5_die "push-data: PS5_HOST must be a host name or address, got '$PS5_HOST'" ;;
esac
# ps5_http reads this; keep it identical to `port` so there is exactly one
# HTTP endpoint in this script.
export PS5_PROSPERO_PORT="$port"

# Path encoding and JSON bodies come from ps5-env.sh (ps5_url_encode,
# ps5_json_path): one encoding for every script that builds a request.

if [ "$mode" = "dry-run" ]; then
    base="http://${PS5_HOST}:${port}"
    say "dry run: no request is sent; every request below would be made"
    dirs=("$REMOTE_ROOT" "$REMOTE_BENCH")
    if [ "$scenes_only" -eq 0 ]; then
        dirs+=("$REMOTE_MODELS")
    fi
    if [ "${#scene_names[@]}" -gt 0 ]; then
        dirs+=("$REMOTE_SCENES")
        for name in "${scene_names[@]}"; do dirs+=("$(scene_dest "$name")"); done
    fi
    printf '  GET  %s/api/status\n' "$base"
    for dir in "${dirs[@]}"; do
        printf '  GET  %s/api/ftp/list?path=%s\n' "$base" "$(ps5_url_encode "$dir")"
    done
    for dir in "${dirs[@]}"; do
        printf '  POST %s/api/ftp/mkdir  body %s   (only if the GET above failed)\n' \
               "$base" "$(ps5_json_path "$dir")"
    done
    for i in "${!plan_name[@]}"; do
        name="${plan_name[$i]}"
        src="${plan_src[$i]}"
        dir="${plan_dir[$i]}"
        total="$(file_size "$src")"
        [ "$total" -gt 0 ] || continue
        upload_id="spikea$(printf '%s' "$dir/$name|$total" | python3 -c 'import hashlib,sys;print(hashlib.sha1(sys.stdin.buffer.read()).hexdigest()[:24])')"
        chunk_bytes=$((chunk_mb * 1024 * 1024))
        offset=0
        part=1
        while [ "$offset" -lt "$total" ]; do
            size="$chunk_bytes"
            [ $((offset + size)) -le "$total" ] || size=$((total - offset))
            printf '  POST %s/api/ftp/upload/chunk?path=%s&upload_id=%s&offset=%d&total=%d&overwrite=1  body %d bytes (chunk %d)\n' \
                   "$base" "$(ps5_url_encode "$dir/$name")" "$upload_id" "$offset" "$total" "$size" "$part"
            offset=$((offset + size))
            part=$((part + 1))
        done
    done
    if [ "$scenes_only" -eq 0 ]; then
        printf '  POST %s/api/ftp/upload/chunk?path=%s&upload_id=...&offset=0&total=<n>&overwrite=1  body < bench.env >\n' \
               "$base" "$(ps5_url_encode "$REMOTE_BENCH/bench.env")"
        printf '  GET  %s/api/ftp/list?path=%s   (final size verify)\n' "$base" "$(ps5_url_encode "$REMOTE_MODELS")"
        printf '  GET  %s/api/ftp/list?path=%s   (final size verify)\n' "$base" "$(ps5_url_encode "$REMOTE_BENCH")"
    fi
    if [ "${#scene_names[@]}" -gt 0 ]; then
        for name in "${scene_names[@]}"; do
            printf '  GET  %s/api/ftp/list?path=%s   (final size verify)\n' "$base" "$(ps5_url_encode "$(scene_dest "$name")")"
        done
    fi
    exit 0
fi

status=$(ps5_http GET /api/status none "") || {
    say "no HTTP answer from ProsperoMgr on $PS5_HOST:$port"
    say "launch ProsperoMgr on the console first (the same service run.sh uses)"
    exit 3
}
if [ "$(printf '%s' "$status" | ps5_json_field name)" != "Prospero Manager" ]; then
    say "the service on $port is not ProsperoMgr; refusing to continue"
    exit 3
fi
say "ProsperoMgr $(printf '%s' "$status" | ps5_json_field version) on $PS5_HOST:$port"

# Remote directory listing -> "name<TAB>size" lines, empty on error.
remote_list() {
    local body
    body=$(ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$1")" none "" 2>/dev/null) || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for e in doc.get("entries", []):
    if not e.get("is_dir"):
        print("%s\t%d" % (e.get("name", ""), int(e.get("size", 0))))
'
}

remote_size() {  # remote_size DIR NAME -> bytes, empty when absent
    remote_list "$1" | awk -F'\t' -v want="$2" '$1 == want {print $2; exit}'
}

ensure_dir() {
    local dir="$1"
    if ! ps5_http GET "/api/ftp/list?path=$(ps5_url_encode "$dir")" none "" >/dev/null 2>&1; then
        say "mkdir $dir"
        ps5_http POST /api/ftp/mkdir json "$(ps5_json_path "$dir")" >/dev/null || {
            say "cannot create $dir"
            exit 1
        }
    fi
}

ensure_dir "$REMOTE_ROOT"
if [ "$scenes_only" -eq 0 ]; then
    ensure_dir "$REMOTE_MODELS"
fi
# The viewer's input file (viewer.env) lives in bench/, so the directory is
# created even for a scenes-only push; the viewer title needs it on its first
# run and run-viewer.sh writes the file there.
ensure_dir "$REMOTE_BENCH"
if [ "${#scene_names[@]}" -gt 0 ]; then
    ensure_dir "$REMOTE_SCENES"
    for name in "${scene_names[@]}"; do
        ensure_dir "$(scene_dest "$name")"
    done
fi

# ---- chunked upload --------------------------------------------------------
# One python3 process per file: 64 MiB requests, up to 3 attempts per chunk,
# resumable through the server's checkpoint. A stable upload_id derived from
# the destination and total size makes the checkpoint survive a dropped
# connection or a re-run; <dest>.pmgr-part-<id> in the directory listing is the
# resume offset. offset 0 is the only way the server truncates, so a stale or
# oversized checkpoint falls back to a clean start.
upload_file() {
    local src_file="$1" remote="$2"
    LOCAL="$src_file" REMOTE="$remote" HOST="$PS5_HOST" PORT="$port" CHUNK_MB="$chunk_mb" \
    python3 - <<'PY'
import hashlib, http.client, json, os, sys, time, urllib.parse

local = os.environ["LOCAL"]
remote = os.environ["REMOTE"]
host = os.environ["HOST"]
port = int(os.environ["PORT"])
chunk = int(os.environ["CHUNK_MB"]) * 1024 * 1024
total = os.path.getsize(local)
directory = os.path.dirname(remote) or "/"
basename = os.path.basename(remote)
upload_id = "spikea" + hashlib.sha1(
    ("%s|%d" % (remote, total)).encode()).hexdigest()[:24]
partial_name = basename + ".pmgr-part-" + upload_id


def say(msg):
    sys.stderr.write("push-data: %s\n" % msg)
    sys.stderr.flush()


def request(method, path, body=None, timeout=300):
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        headers = {"Content-Type": "application/octet-stream"} if body is not None else {}
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, data
    finally:
        conn.close()


def list_dir(path):
    status, data = request("GET", "/api/ftp/list?path=" +
                           urllib.parse.quote(path, safe=""))
    if status < 200 or status >= 300:
        return {}
    try:
        doc = json.loads(data.decode())
    except Exception:
        return {}
    return {e.get("name", ""): int(e.get("size", 0))
            for e in doc.get("entries", []) if not e.get("is_dir")}


def checkpoint_offset():
    """Resume offset from the server-side checkpoint, or 0."""
    entries = list_dir(directory)
    size = entries.get(partial_name)
    if size is None:
        return 0
    if 0 < size < total:
        say("%s: resuming at %d of %d bytes from %s"
            % (remote, size, total, partial_name))
        return size
    if size >= total:
        say("%s: discarding a stale %d-byte checkpoint" % (remote, size))
    return 0


def query(offset):
    return "/api/ftp/upload/chunk?" + urllib.parse.urlencode({
        "path": remote,
        "upload_id": upload_id,
        "offset": offset,
        "total": total,
        "overwrite": "1",
    })


def transfer(fileobj, offset):
    """Send one chunk; returns (new_offset, complete)."""
    fileobj.seek(offset)
    body = fileobj.read(chunk)
    if not body:
        raise RuntimeError("short read at offset %d" % offset)
    for attempt in range(1, 4):
        try:
            status, data = request("POST", query(offset), body)
        except OSError as error:
            say("chunk at %d: %s (attempt %d/3)" % (offset, error, attempt))
            time.sleep(0.5 * attempt)
            continue
        if status == 409:
            # The checkpoint moved (a lost reply or a concurrent writer): ask
            # the server for its size and continue from there.
            resume = checkpoint_offset()
            raise Checkpoint(resume)
        if status < 200 or status >= 300:
            say("chunk at %d: HTTP %d %s (attempt %d/3)"
                % (offset, status, data[:200].decode("utf-8", "replace"), attempt))
            time.sleep(0.5 * attempt)
            continue
        doc = json.loads(data.decode())
        nxt = int(doc.get("next_offset", -1))
        if nxt != offset + len(body):
            raise RuntimeError("server reported next_offset %d, expected %d"
                               % (nxt, offset + len(body)))
        return nxt, bool(doc.get("complete"))
    raise RuntimeError("chunk at %d failed after 3 attempts" % offset)


class Checkpoint(Exception):
    def __init__(self, offset):
        super().__init__("checkpoint")
        self.offset = offset


started = time.time()
offset = checkpoint_offset()
done = False
last_report = 0.0
stalls = 0
try:
    with open(local, "rb") as f:
        while offset < total:
            try:
                offset, done = transfer(f, offset)
            except Checkpoint as moved:
                # No progress at the same offset means the 409 is not a stale
                # checkpoint (e.g. another writer holds the reservation); stop
                # instead of spinning.
                if moved.offset == offset:
                    stalls += 1
                    if stalls > 5:
                        raise RuntimeError("checkpoint kept reporting offset %d" % offset)
                else:
                    stalls = 0
                offset = moved.offset
                continue
            if done:
                break
            now = time.time()
            if now - last_report >= 5.0:
                pct = 100.0 * offset / total
                say("%s: %.1f%% (%d of %d bytes, %.1f MiB/s)"
                    % (remote, pct, offset, total,
                       offset / 1048576.0 / max(now - started, 1e-6)))
                last_report = now
except RuntimeError as error:
    say("%s: %s" % (remote, error))
    sys.exit(1)

# Verify what is now at the destination, not just what the last reply said.
for attempt in range(1, 4):
    entries = list_dir(directory)
    if entries.get(basename) == total:
        say("%s: complete and verified, %d bytes in %.1f s"
            % (remote, total, time.time() - started))
        sys.exit(0)
    if attempt < 3:
        time.sleep(0.5 * attempt)
say("%s: upload finished but the destination size is %s, expected %d"
    % (remote, entries.get(basename), total))
sys.exit(1)
PY
}

# ---- run -------------------------------------------------------------------
failed=0
for i in "${!plan_name[@]}"; do
    name="${plan_name[$i]}"
    src="${plan_src[$i]}"
    dir="${plan_dir[$i]}"
    local_bytes=$(file_size "$src")
    remote_bytes=$(remote_size "$dir" "$name" || true)
    if [ "$mode" = "verify" ]; then
        if [ "$local_bytes" = "$remote_bytes" ]; then
            printf '  OK   %-34s %10s\n' "$name" "$(human "$local_bytes")"
        else
            printf '  DIFF %-34s local %s, remote %s\n' "$name" "$(human "$local_bytes")" \
                   "$( [ -n "$remote_bytes" ] && human "$remote_bytes" || echo missing )"
            failed=1
        fi
        continue
    fi
    if [ -n "$remote_bytes" ] && [ "$local_bytes" = "$remote_bytes" ]; then
        say "$name: already on the console ($(human "$local_bytes")), skipping"
        continue
    fi
    if [ -n "$remote_bytes" ]; then
        say "$name: remote is $(human "$remote_bytes"), local $(human "$local_bytes"): replacing "\
"only after the new upload is complete"
    else
        say "$name: uploading $(human "$local_bytes") -> $dir/$name"
    fi
    upload_file "$src" "$dir/$name" || failed=1
done

if [ "$mode" = "verify" ]; then
    [ "$failed" -eq 0 ] && say "verify: PASS (all sizes match)" || say "verify: FAIL"
    exit "$failed"
fi

# bench.env: the payloads read it before their first GPU call. Default content
# leaves RADV at its own settings; op_audit now flips coopmat itself, so this
# file is only the override for a forced run. A scenes-only push leaves this
# file alone (it only creates the bench/ directory the viewer's input lives in).
bench_env="$REMOTE_BENCH/bench.env"
if [ "$scenes_only" -eq 0 ] && [ -z "$(remote_size "$REMOTE_BENCH" "bench.env")" ]; then
    tmp="$(mktemp "${TMPDIR:-/tmp}/bench.env.XXXXXX")"
    cat >"$tmp" <<'EOF'
# Read by op_audit/da2_bench/sharp_bench before the first Vulkan call.
# op_audit measures coopmat on and off itself; leave this unset for that
# comparison. Set it to force the disabled path on every payload:
#GGML_VK_DISABLE_COOPMAT=1
# Cap ggml-vulkan's per-submit node count (default 100) if submits time out:
#GGML_VK_MAX_NODES_PER_SUBMIT=64
EOF
    say "writing $bench_env"
    upload_file "$tmp" "$bench_env" || failed=1
    # Local mktemp cleanup only; this script has no remote delete call.
    rm -f "$tmp"
fi

say "remote layout:"
if [ "$scenes_only" -eq 0 ]; then
    remote_list "$REMOTE_MODELS" | sed 's/^/  models\//' >&2 || true
    remote_list "$REMOTE_BENCH" | sed 's/^/  bench\//' >&2 || true
fi
for name in "${scene_names[@]+"${scene_names[@]}"}"; do
    remote_list "$(scene_dest "$name")" | sed "s/^/  scenes\\/$name\\//" >&2 || true
done

if [ "$failed" -ne 0 ]; then
    say "FAIL one or more uploads did not complete; re-run to resume (the server-side checkpoint is reused)"
    exit 1
fi
say "done. Next on the console:"
if [ "${#scene_names[@]}" -gt 0 ]; then
    say "  platform/ps5/scripts/run-viewer.sh            # splats on the TV"
fi
if [ "$scenes_only" -eq 0 ]; then
    say "  platform/ps5/scripts/run.sh platform/ps5/bench/op_audit.elf 1200"
    say "  platform/ps5/scripts/run.sh platform/ps5/bench/da2_bench.elf 300"
    say "  platform/ps5/scripts/run.sh platform/ps5/bench/sharp_bench.elf 900"
fi
exit 0
