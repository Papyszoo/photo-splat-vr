#!/usr/bin/env bash
# Uninstall our homebrew titles from the console completely — dry run by default.
#
#   platform/ps5/scripts/title-uninstall.sh PPSA99202
#   platform/ps5/scripts/title-uninstall.sh --yes PPSA99202
#   platform/ps5/scripts/title-uninstall.sh --yes --data PPSA99202
#
# What an install leaves on the console (title.sh install -> ShadowMountPlus):
#   /data/homebrew/<ID>/          our files, uploaded by title.sh (the source)
#   /user/appmeta/<ID>/           sce_sys metadata copied by ShadowMountPlus
#   /user/app/<ID>/               staged sce_sys + icon0.png + mount.lnk
#   /system_ex/app/<ID>           nullfs mount of the source (runtime)
#   app.db rows                   tbl_contentinfo + tbl_appbrowse (the tile)
#   /data/shadowmount/debug.log   log lines only; its per-title counters are RAM
# This script removes the first and asks the system to remove the rest through
# the supported unregister call (ShadowMountPlus -> sceAppInstUtilAppUnInstall).
#
# Order, per title: stop it if it is the running one -> unregister it ->
# remove /data/homebrew/<ID>/ -> verify.
#
# Safety:
#   - hard allowlist: the id must be a titleId in
#     platform/ps5/title/identities.json (the dev and the release build). An id
#     in neither entry is refused before a single request is sent.
#   - ownership proof from the param.json that is on the console (homebrew
#     folder, appmeta or /user/app staging copy): it must carry that titleId and
#     one of that identity's own names — for PPSA99202 exactly "Photo Splat VR
#     Dev" (this build) or "PS5 Splat VR Viewer" (the build installed before the
#     dev name), for PPSA99770 exactly "Photo Splat VR". A foreign title that
#     took an id (Twiso on PPSA99202) is refused, another identity's name is
#     refused, and nothing under its folder is deleted.
#   - the bare id must look like PPSA9#### (one homebrew title id) before any
#     request is built; every id is checked before the first action, and one bad
#     id stops all of them.
#   - the only tree this script deletes is exactly /data/homebrew/<ID> (one
#     POST /api/ftp/delete per id: no wildcard, no recursion of our own) and,
#     only with --data, exactly the data folder of the dev identity
#     (/data/ps5-splat-vr). --data is REFUSED for PPSA99770: its folder is the
#     user's private data and no script in this repo deletes it (this is the
#     same hard guard the other console scripts use).
#   - /user/app/<ID>, /user/appmeta/<ID> and the /system_ex mount point are
#     never deleted here: they are the system's, not ours. They are checked and
#     reported (exit 6 when something is left behind).
#   - the dry run sends read-only GETs only, so the allowlist check and the plan
#     are the real ones; it never uploads, unregisters or deletes.
#   - the unregister calls ShadowMountPlus on 10101. If that port does not answer
#     (its default is api_bind_address=127.0.0.1, loopback only), no --yes run
#     starts at all: removing the folder first would leave a registered tile
#     pointing at a missing source.
#
# Settings: PS5_HOST, PS5_PROSPERO_PORT (7070), PS5_SHADOWMOUNT_PORT (10101),
# PS5_TITLE_WAIT (90 s for the title list), PS5_TITLE_QUIT_WAIT (20),
# PS5_TITLE_DELETE_WAIT (180 s for the delete job). Snapshots go to
# $SPLAT_DATA_DIR/runs/console_cleanup/<UTC>-<mode>-before.json / -after.json.
#
# Exit: 0 unregistered and removed, nothing left behind; 1 a request failed or
# the before/after difference was not exactly the requested ids; 2 usage, an id
# that is not ours, or a console copy that is not ours; 3 no ProsperoMgr or no
# ShadowMountPlus answer (nothing was changed); 4 the unregister was refused or
# the title stayed in the list; 5 the title was running and would not stop; 6
# unregistered and removed, but per-title leftovers remain (listed).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
# shellcheck source=ps5-env.sh
. "$here/ps5-env.sh"

usage() {
    sed -n '2,/^# SPDX-License-Identifier/p' "${BASH_SOURCE[0]}" |
        sed -e '$d' -e 's/^# \{0,1\}//'
}

# ---- arguments -------------------------------------------------------------
mode="dry-run"
want_data=0
files_only=0
ids=""
for argument in "$@"; do
    case "$argument" in
        --yes|-y) mode="run" ;;
        # Remove our folders but leave the tiles: the user deletes them on the
        # console (Options > Delete). Needs no ShadowMountPlus API; with the
        # folder gone its scan cannot re-add the tile.
        --files-only) files_only=1 ;;
        --dry-run) mode="dry-run" ;;
        --data) want_data=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) printf 'title-uninstall: unknown option %s\n' "$argument" >&2; usage >&2; exit 2 ;;
        *) ids="$ids $argument" ;;
    esac
done
[ -n "$ids" ] || { usage >&2; exit 2; }

ps5_env_load

say() { printf 'title-uninstall: %s\n' "$*" >&2; }
fail() { say "$1"; exit "${2:-1}"; }

prospero_port="${PS5_PROSPERO_PORT:-7070}"
shadowmount_port="${PS5_SHADOWMOUNT_PORT:-10101}"
title_wait="${PS5_TITLE_WAIT:-90}"
quit_wait="${PS5_TITLE_QUIT_WAIT:-20}"
delete_wait="${PS5_TITLE_DELETE_WAIT:-180}"
export PS5_HTTP_TIMEOUT="${PS5_HTTP_TIMEOUT:-60}"

data_dir="$(ps5_data_dir)/runs/console_cleanup"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"

# ---- the allowlist: this repo's own title ids and names --------------------
# platform/ps5/title/identities.json is the one table. Each entry carries the
# exact names its install may show (titleName plus any previousTitleNames), and
# the proof below accepts that id only with one of its own names, so a foreign
# title that took an id (Twiso) is never ours and another identity's name never
# proves this one. An id in neither entry is refused before a single request is
# sent, and nothing under any folder is deleted without the ownership proof.
allow="$(python3 "$repo/platform/ps5/title/identity.py" --table)" ||
    fail "cannot read platform/ps5/title/identities.json" 2
[ -n "$allow" ] || fail "no identity in platform/ps5/title/identities.json" 2

is_allowed() {
    printf '%s\n' "$allow" | awk -F'\t' -v want="$1" '$1 == want { found = 1 } END { exit !found }'
}

# is_proven ID: the console copy's param.json already showed this id is ours.
is_proven() {
    case "$proven" in *" $1 "*) return 0 ;; esac
    return 1
}

# is_private_path PATH: the path is (or is under) a folder agents must not read,
# list, write or delete — the release identity's data folder. Decided from the
# table, before any request, so nothing here can list it.
is_private_path() {
    local dir
    for dir in $(ps5_private_data_dirs); do
        case "$1" in
            "$dir"|"$dir"/*) return 0 ;;
        esac
    done
    return 1
}

ids="$(python3 - $ids <<'PY'
import sys
seen = []
for value in sys.argv[1:]:
    if value not in seen:
        seen.append(value)
print(" ".join(seen))
PY
)"

for title_id in $ids; do
    case "$title_id" in
        PPSA9[0-9][0-9][0-9][0-9]) ;;
        *) fail "'$title_id' is not a homebrew title id (PPSA9####); nothing was sent" 2 ;;
    esac
    # An id in neither entry is not ours at all: refuse before any request.
    is_allowed "$title_id" ||
        fail "refusing $title_id: it is not in platform/ps5/title/identities.json ($(printf '%s' "$allow" | cut -f1 | tr '\n' ' ')); nothing was sent" 2
done

# The data folder each id owns, and the --data policy, decided from the table
# before any request: --data may remove only the dev folder. The release
# identity's folder is the user's private data and is never deleted, so
# `--data PPSA99770` stops here with nothing sent. A run removes one folder.
data_root=""
for title_id in $ids; do
    root="$(ps5_title_field dataDir --title-id "$title_id" 2>/dev/null)" || root=""
    [ -n "$root" ] ||
        fail "refusing $title_id: it is not in platform/ps5/title/identities.json; nothing was sent" 2
    if [ "$want_data" = 1 ]; then
        case " $(ps5_private_data_dirs | tr '\n' ' ') " in
            *" $root "*)
                fail "refusing --data $title_id: $root is the user's private data folder and this repo's scripts never delete it. Nothing was sent (run without --data to remove the package files only)" 2 ;;
        esac
        [ -z "$data_root" ] || [ "$data_root" = "$root" ] ||
            fail "--data with more than one data folder ($data_root and $root); one run removes one folder. Nothing was sent" 2
    fi
    [ -n "$data_root" ] || data_root="$root"
done
[ -n "$data_root" ] || data_root="$(ps5_title_field dataDir --name dev 2>/dev/null || true)"

[ -n "${PS5_HOST:-}" ] || fail "set PS5_HOST or add PS5_HOST=... to .env.local" 2

# ---- request helpers -------------------------------------------------------
# Read-only GETs are always sent, in both modes, so the allowlist check and the
# plan are the real ones. Writes are printed in a dry run and only sent with
# --yes. The transcript goes to stderr, the body to stdout, so a dry run is a
# readable wire log and `| python3` still parses the answer.
json_title() { python3 -c 'import json,sys;print(json.dumps({"title_id":sys.argv[1]}))' "$1"; }
api() { printf 'http://%s:%s' "$PS5_HOST" "$prospero_port"; }
sm_api() { printf 'http://%s:%s' "$PS5_HOST" "$shadowmount_port"; }

get_http() {  # get_http PATH -> body (empty when the route refuses)
    printf '  GET  %s%s\n' "$(api)" "$1" >&2
    ps5_http GET "$1" none "" 2>/dev/null
}

jobs_state() {  # jobs_state JOB_ID -> the job's state word
    printf '  GET  %s/api/files/jobs\n' "$(api)" >&2
    ps5_http GET /api/files/jobs none "" 2>/dev/null | python3 -c '
import json, sys
want = sys.argv[1]
try:
    doc = json.load(sys.stdin)
except Exception:
    print("unknown"); sys.exit(0)
for entry in doc.get("jobs", []):
    if str(entry.get("id")) == want:
        print(entry.get("state", "unknown"))
        break
else:
    print("unknown")
' "$1"
}

write_http() {  # write_http METHOD PATH KIND DATA NOTE -> body (empty in a dry run)
    local method="$1" path="$2" kind="$3" data="${4:-}" note="${5:-}"
    if [ "$mode" = "dry-run" ]; then
        printf '  %-4s %s%s  body %s   (dry run: not sent%s)\n' \
               "$method" "$(api)" "$path" "$data" "${note:+; $note}" >&2
        return 0
    fi
    printf '  %-4s %s%s  body %s%s\n' \
           "$method" "$(api)" "$path" "$data" "${note:+   ($note)}" >&2
    ps5_http "$method" "$path" "$kind" "$data"
}

sm_reachable() { ps5_port_open "$PS5_HOST" "$shadowmount_port" 3; }

sm_post() {  # sm_post JSON NOTE -> body (empty in a dry run)
    local body="$1" note="${2:-}"
    if [ "$mode" = "dry-run" ]; then
        printf '  POST %s/api/v1/games/uninstall  body %s   (dry run: not sent%s)\n' \
               "$(sm_api)" "$body" "${note:+; $note}" >&2
        return 0
    fi
    printf '  POST %s/api/v1/games/uninstall  body %s%s\n' \
           "$(sm_api)" "$body" "${note:+   ($note)}" >&2
    printf '%s' "$body" | PS5_SM_HOST="$PS5_HOST" PS5_SM_PORT="$shadowmount_port" \
    PS5_SM_TIMEOUT="${PS5_SM_TIMEOUT:-30}" python3 -c '
import http.client, os, sys
body = sys.stdin.read()
conn = http.client.HTTPConnection(os.environ["PS5_SM_HOST"],
                                  int(os.environ["PS5_SM_PORT"]),
                                  timeout=float(os.environ["PS5_SM_TIMEOUT"]))
try:
    conn.request("POST", "/api/v1/games/uninstall", body=body,
                 headers={"Content-Type": "application/json"})
    response = conn.getresponse()
    payload = response.read().decode("utf-8", "replace")
    sys.stdout.write(payload)
    sys.stdout.flush()
    if response.status != 200:
        sys.stderr.write("\nHTTP %d %s\n" % (response.status, payload[:300]))
        sys.exit(1)
except OSError as error:
    sys.stderr.write("%s: %s\n" % (type(error).__name__, error))
    sys.exit(1)
finally:
    conn.close()
'
}

# ---- read helpers ----------------------------------------------------------
list_dir() {  # list_dir PATH -> "name<TAB>dir|file<TAB>size", empty when absent
    local body
    body="$(get_http "/api/ftp/list?path=$(ps5_url_encode "$1")")" || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for entry in doc.get("entries", []):
    print("%s\t%s\t%d" % (entry.get("name", ""),
                          "dir" if entry.get("is_dir") else "file",
                          int(entry.get("size", 0))))
'
}

path_exists() {  # path_exists PATH -> true when the parent listing has the name
    local parent name
    parent="$(dirname -- "$1")"
    name="$(basename -- "$1")"
    list_dir "$parent" | awk -F'\t' -v want="$name" '$1 == want { found = 1 } END { exit !found }'
}

dir_size() {  # dir_size PATH -> bytes, nothing when absent
    local body
    body="$(get_http "/api/ftp/dirsize?path=$(ps5_url_encode "$1")")" || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    print(int(json.load(sys.stdin).get("size", 0)))
except Exception:
    pass
'
}

read_text() {  # read_text PATH -> file text, empty when absent
    local body
    body="$(get_http "/api/ftp/text?path=$(ps5_url_encode "$1")")" || return 0
    printf '%s' "$body" | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("text", ""), end="")
except Exception:
    pass
'
}

title_ids() {  # title_ids -> one id per line (ProsperoMgr reads app.db)
    local body
    body="$(get_http /api/titles)" || return 0
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

running_title() {  # running_title -> the running id, empty when none
    local body
    body="$(get_http /api/titles/running)" || return 0
    printf '%s' "$body" | ps5_json_field title_id || true
}

human() {  # human BYTES
    python3 -c '
import sys
raw = sys.stdin.read().strip()
if not raw:
    print("absent")
    raise SystemExit
n = float(raw); units = ["B", "KiB", "MiB", "GiB"]; i = 0
while n >= 1024 and i < 3:
    n /= 1024; i += 1
print("%.2f %s" % (n, units[i]))'
}

# ---- ownership check -------------------------------------------------------
# ours_check ID TEXT -> "yes" when that installed param.json proves the id is
# ours: it must carry that titleId and one of that identity's own names (for
# PPSA99202 ""Photo Splat VR Dev"" or the name of the build installed before it;
# for PPSA99770 ""Photo Splat VR""). The decision lives in
# platform/ps5/title/identity.py so the offline test
# (platform/ps5/title/ownership-test.sh) checks the same code a console run uses.
ours_check() {
    local answer
    answer="$(printf '%s' "$2" | python3 "$repo/platform/ps5/title/identity.py" --owns "$1")" ||
        return 1
    [ "$answer" = "yes" ] && printf 'yes\n'
    return 0
}

# ---- preflight -------------------------------------------------------------
say "mode: $mode"
if [ "$want_data" = 1 ]; then say "--data: $data_root is removed as well (the dev identity's folder)"; fi
say "titles: $(printf '%s' "$ids")"
mkdir -p "$data_dir"

status_body="$(get_http /api/status)" ||
    fail "no HTTP answer from ProsperoMgr on $PS5_HOST:$prospero_port; nothing was sent" 3
service="$(printf '%s' "$status_body" | ps5_json_field name)"
[ "$service" = "Prospero Manager" ] ||
    fail "the service on $prospero_port is '$service', not ProsperoMgr; refusing (nothing was sent)" 3
say "ProsperoMgr $(printf '%s' "$status_body" | ps5_json_field version) on $PS5_HOST:$prospero_port"

sm_up=0
if sm_reachable; then
    sm_up=1
    say "ShadowMountPlus API reachable on $PS5_HOST:$shadowmount_port"
else
    say "ShadowMountPlus API NOT reachable on $PS5_HOST:$shadowmount_port"
    say "  its config.ini default is api_bind_address=127.0.0.1 (loopback only; this"
    say "  console logs api=127.0.0.1:10101), so the uninstall request cannot be sent"
    say "  from the Mac. Set api_bind_address to the console's LAN address (or 0.0.0.0)"
    say "  in /data/shadowmount/config.ini — it reloads on the file stamp — then rerun."
fi
if [ "$files_only" = 1 ]; then
    say "--files-only: no unregister request; the tiles stay until deleted on the console"
fi
if [ "$mode" = "run" ] && [ "$sm_up" = 0 ] && [ "$files_only" = 0 ]; then
    say "refusing to remove anything: an unregister that cannot be sent would leave"
    say "a registered tile whose files are gone. Nothing was changed."
    exit 3
fi

# The repo table says which ids may be touched; the installed param.json says
# whose install this actually is. Both must agree, for every id, before any write.
# An id is proved only by that titleId plus one of that identity's own names, so
# a foreign title that took the id (Twiso) is refused here, before any write.
proven=" "
for title_id in $ids; do
    found=""
    foreign=""
    primary="/data/homebrew/$title_id/sce_sys/param.json"
    if path_exists "$primary"; then
        # The source folder is there: its param.json decides, and a mismatch is
        # final — no fallback to a metadata copy that an earlier install left.
        text="$(read_text "$primary")"
        [ "$(ours_check "$title_id" "$text")" = "yes" ] ||
            fail "refusing $title_id: $primary is not ours (titleId/titleName do not match platform/ps5/title/identities.json); nothing was sent" 2
        found="$primary"
    else
        for candidate in "/user/appmeta/$title_id/param.json" \
                         "/user/app/$title_id/sce_sys/param.json"; do
            text="$(read_text "$candidate")"
            [ -n "$text" ] || continue
            if [ "$(ours_check "$title_id" "$text")" = "yes" ]; then
                found="$candidate"
                break
            fi
            foreign="$candidate"
        done
    fi
    if [ -n "$found" ]; then
        say "$title_id: ours ($found carries our titleId and titleName)"
        proven="$proven$title_id "
        continue
    fi
    if title_ids | grep -qx "$title_id"; then
        fail "refusing $title_id: it is in the title list but no param.json with our titleId and titleName was found under /data/homebrew/$title_id/sce_sys, /user/appmeta/$title_id or /user/app/$title_id/sce_sys; nothing was sent" 2
    fi
    if [ -n "$foreign" ]; then
        fail "refusing $title_id: $foreign is not ours (titleId/titleName do not match platform/ps5/title/identities.json); nothing was sent" 2
    fi
    if path_exists "/data/homebrew/$title_id"; then
        fail "refusing $title_id: /data/homebrew/$title_id exists but no param.json proves it is ours; nothing was sent" 2
    fi
    say "$title_id: not registered and no param.json — already uninstalled"
done

# ---- plan ------------------------------------------------------------------
current="$(running_title || true)"
if [ -n "$current" ]; then say "running now: $current"; fi

printf '\nplan (%s):\n' "$mode" >&2
for title_id in $ids; do
    registered=no
    title_ids | grep -qx "$title_id" && registered=yes
    size="$(dir_size "/data/homebrew/$title_id")"
    printf '  %s  registered=%-3s /data/homebrew/%s %s\n' \
           "$title_id" "$registered" "$title_id" "$(printf '%s' "$size" | human)" >&2
done
if [ "$want_data" = 1 ]; then
    printf '  %s %s (--data: scenes, bench output, viewer.env, title-quit)\n' \
           "$data_root" "$(dir_size "$data_root" | human)" >&2
fi
printf '\n' >&2

# ---- snapshots -------------------------------------------------------------
snapshot() {  # snapshot LABEL -> path of the JSON it wrote
    local label="$1"
    local path titles per data_present=no
    path="$data_dir/$stamp-$mode-$label.json"
    # The release identity's folder is never listed here (is_private_path): the
    # snapshot records "no" instead of reading the user's private data.
    if [ -n "$data_root" ] && ! is_private_path "$data_root" && path_exists "$data_root"; then
        data_present=yes
    fi
    titles="$(get_http /api/titles)" ||
        fail "could not read the title list for the $label snapshot" 1
    per=""
    for title_id in $ids; do
        per="$per$(printf '%s\t%s\t%s\t%s\t%s' "$title_id" \
                 "$(path_exists "/data/homebrew/$title_id" && echo yes || echo no)" \
                 "$(path_exists "/user/app/$title_id" && echo yes || echo no)" \
                 "$(path_exists "/user/appmeta/$title_id" && echo yes || echo no)" \
                 "$(list_dir "/system_ex/app/$title_id" | cut -f1 | tr '\n' ' ')")"$'\n'
    done
    TITLE_U_PATH="$path" TITLE_U_LABEL="$label" TITLE_U_MODE="$mode" \
    TITLE_U_TITLES="$titles" TITLE_U_PER="$per" \
    TITLE_U_HOMEBREW="$(list_dir /data/homebrew)" \
    TITLE_U_DATA="$data_present" \
    python3 - <<'PY'
import json, os

def lines(text):
    return [line for line in text.splitlines() if line]

homebrew = []
for line in lines(os.environ["TITLE_U_HOMEBREW"]):
    parts = line.split("\t")
    if len(parts) >= 3:
        homebrew.append({"name": parts[0], "kind": parts[1], "size": int(parts[2])})

per_title = {}
for line in lines(os.environ["TITLE_U_PER"]):
    parts = line.split("\t")
    per_title[parts[0]] = {
        "homebrew_dir": parts[1] == "yes",
        "user_app": parts[2] == "yes",
        "appmeta": parts[3] == "yes",
        "system_ex_entries": [name for name in parts[4].split(" ") if name],
    }

try:
    titles = json.loads(os.environ["TITLE_U_TITLES"])
except Exception:
    titles = {"titles": []}

doc = {
    "label": os.environ["TITLE_U_LABEL"],
    "mode": os.environ["TITLE_U_MODE"],
    "title_list": [entry.get("title_id", "") for entry in titles.get("titles", [])],
    "titles_raw": titles,
    "homebrew": homebrew,
    "data_dir_present": os.environ["TITLE_U_DATA"] == "yes",
    "per_title": per_title,
}
with open(os.environ["TITLE_U_PATH"], "w") as out:
    json.dump(doc, out, indent=2, sort_keys=True)
    out.write("\n")
PY
    printf '%s\n' "$path"
}

before_path="$(snapshot before)" || exit 1
say "snapshot: $before_path"

# ---- act, one title at a time ----------------------------------------------
for title_id in $ids; do
    printf '\n== %s\n' "$title_id" >&2

    # 1. stop it, if it is the running one. Another title is never touched.
    current="$(running_title || true)"
    if [ "$current" = "$title_id" ]; then
        say "$title_id is running; asking it to exit through the quit file (never a kill)"
        quit_root="$(ps5_title_field dataDir --title-id "$title_id" 2>/dev/null)" || quit_root=""
        # The release install's folder is the user's private data: never write it,
        # even to ask that title to quit. The console closes it instead.
        if is_private_path "$quit_root"; then
            fail "$title_id is running and its data folder ($quit_root) is the user's private data; close it on the console (PS, Options, Close App) and rerun. Nothing was unregistered or removed" 5
        fi
        [ -n "$quit_root" ] || fail "no data folder for $title_id in platform/ps5/title/identities.json; nothing was removed" 2
        quit_local="$(mktemp "${TMPDIR:-/tmp}/ps5splat-uninstall.XXXXXX")"
        printf 'quit\n' >"$quit_local"
        write_http POST "/api/ftp/upload?path=$(ps5_url_encode "$quit_root/title-quit")&overwrite=1" \
                   file "$quit_local" "5 bytes of \"quit\"; the title ends itself" >/dev/null ||
            { rm -f "$quit_local"; fail "could not write $quit_root/title-quit" 1; }
        rm -f "$quit_local"
        if [ "$mode" = "run" ]; then
            waited=0
            while [ "$waited" -lt "$quit_wait" ] && [ "$(running_title || true)" = "$title_id" ]; do
                sleep 2
                waited=$((waited + 2))
            done
            [ "$(running_title || true)" = "$title_id" ] &&
                fail "$title_id is still running after ${quit_wait}s; close it on the console (PS, Options, Close App) and rerun. Nothing was unregistered or removed" 5
            say "stopped: $title_id is no longer running"
        fi
    else
        say "not running (running title: ${current:-none}); no quit file"
    fi

    # 2. unregister through ShadowMountPlus -> sceAppInstUtilAppUnInstall, and
    #    wait for the app.db row to disappear from the title list.
    listed="$(title_ids)"
    # An empty list means the console did not answer, not "not registered":
    # going on would remove the folder and orphan a registered tile.
    if [ -z "$listed" ] && [ "$mode" = "run" ] && [ "$files_only" = 0 ]; then
        fail "could not read the title list; $title_id was not unregistered or removed" 1
    fi
    if [ "$files_only" = 1 ]; then
        say "$title_id: --files-only, no unregister; delete its tile on the console afterwards"
    elif printf '%s\n' "$listed" | grep -qx "$title_id"; then
        sm_post "$(json_title "$title_id")" "the supported unregister call" >/dev/null || {
            say "the uninstall request was refused (EBUSY: a title is active, or a"
            say "runtime mount is prepared). Nothing was removed."
            fail "unregister of $title_id failed" 4
        }
        if [ "$mode" = "run" ]; then
            waited=0
            registered=yes
            while [ "$waited" -lt "$title_wait" ]; do
                title_ids | grep -qx "$title_id" || { registered=no; break; }
                sleep 2
                waited=$((waited + 2))
            done
            [ "$registered" = "no" ] ||
                fail "$title_id is still in GET /api/titles after ${title_wait}s (the app.db row is there); /data/homebrew/$title_id was NOT removed" 4
            say "unregistered: $title_id is gone from the title list"
        fi
    else
        say "$title_id is not in the title list; no unregister request"
    fi

    # 3. remove exactly /data/homebrew/<ID>, and nothing else.
    if path_exists "/data/homebrew/$title_id"; then
        case "$title_id" in
            PPSA9[0-9][0-9][0-9][0-9]) ;;
            *) fail "internal guard: '$title_id' is not a title id" 2 ;;
        esac
        # Nothing is deleted without the ownership proof above: a folder whose
        # param.json was not ours (Twiso, another identity's name) stays.
        if ! is_proven "$title_id"; then
            say "$title_id: not ours (no proof in a param.json); leaving /data/homebrew/$title_id alone"
            continue
        fi
        remove_path="/data/homebrew/$title_id"
        say "remove $remove_path (one delete job, that exact path)"
        job="$(write_http POST /api/ftp/delete json "$(ps5_json_path "$remove_path")" \
                         "removes this path and nothing else" | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    sys.exit(0)
try:
    print(json.loads(raw).get("job_id", ""))
except Exception:
    pass
' || true)"
        if [ "$mode" = "run" ]; then
            [ -n "$job" ] || fail "the delete request for $remove_path was refused (another file job active?)" 1
            waited=0
            state="unknown"
            while [ "$waited" -lt "$delete_wait" ]; do
                state="$(jobs_state "$job" || echo unknown)"
                case "$state" in done|error|attention|canceled) break ;; esac
                sleep 1
                waited=$((waited + 1))
            done
            [ "$state" = "done" ] ||
                fail "the delete job for $remove_path ended '$state'; check GET /api/files/jobs" 1
            say "removed $remove_path"
        fi
    else
        say "/data/homebrew/$title_id is already absent; nothing to remove"
    fi
done

# 4. --data: exactly the dev identity's folder, only when it was asked for.
# The table refused the release identity's folder before the first request.
if [ "$want_data" = 1 ]; then
    printf '\n== %s (--data)\n' "$data_root" >&2
    if path_exists "$data_root"; then
        say "remove $data_root (scenes must be pushed again)"
        job="$(write_http POST /api/ftp/delete json "$(ps5_json_path "$data_root")" \
                         "the whole data directory" | python3 -c '
import json, sys
raw = sys.stdin.read().strip()
try:
    print(json.loads(raw).get("job_id", ""))
except Exception:
    pass
' || true)"
        if [ "$mode" = "run" ]; then
            [ -n "$job" ] || fail "the delete request for $data_root was refused" 1
            waited=0
            state="unknown"
            while [ "$waited" -lt "$delete_wait" ]; do
                state="$(jobs_state "$job" || echo unknown)"
                case "$state" in done|error|attention|canceled) break ;; esac
                sleep 1
                waited=$((waited + 1))
            done
            [ "$state" = "done" ] ||
                fail "the delete job for $data_root ended '$state'; check GET /api/files/jobs" 1
            say "removed $data_root"
        fi
    else
        say "$data_root is already absent"
    fi
fi

# ---- verification ----------------------------------------------------------
# A console that stopped answering would make every listing look empty; require
# it to answer once more before the snapshots are compared.
printf '\n== verification\n' >&2
final_status="$(get_http /api/status)" || final_status=""
[ "$(printf '%s' "$final_status" | ps5_json_field name)" = "Prospero Manager" ] ||
    fail "ProsperoMgr stopped answering on $PS5_HOST:$prospero_port; the comparison below would be meaningless. Check the console before trusting anything above" 3

after_path="$(snapshot after)" || exit 1
say "snapshot: $after_path"

TITLE_U_BEFORE="$before_path" TITLE_U_AFTER="$after_path" TITLE_U_IDS="$ids" \
TITLE_U_MODE="$mode" TITLE_U_DATA="$want_data" TITLE_U_FILES="$files_only" \
TITLE_U_DATA_ROOT="$data_root" python3 - <<'PY' || exit $?
import json, os, sys

before = json.load(open(os.environ["TITLE_U_BEFORE"]))
after = json.load(open(os.environ["TITLE_U_AFTER"]))
ids = os.environ["TITLE_U_IDS"].split()
wanted = set(ids)
dry = os.environ["TITLE_U_MODE"] == "dry-run"
want_data = os.environ["TITLE_U_DATA"] == "1"
files_only = os.environ["TITLE_U_FILES"] == "1"

problems, leftovers, lines = [], [], []

def report(kind, message):
    lines.append("  %-5s %s" % (kind, message))

old_titles, new_titles = set(before["title_list"]), set(after["title_list"])
removed, added = sorted(old_titles - new_titles), sorted(new_titles - old_titles)

if dry:
    if removed or added:
        problems.append("the console changed during the dry run (removed %s, added %s)"
                        % (", ".join(removed) or "none", ", ".join(added) or "none"))
    report("ok" if not problems else "FAIL",
           "title list unchanged (%d title(s)); nothing was sent" % len(new_titles))
else:
    unexpected = [title for title in removed if title not in wanted]
    # --files-only leaves the tiles on purpose: a requested id may stay listed.
    still = [] if files_only else sorted((wanted & old_titles) - set(removed))
    if unexpected:
        problems.append("titles left the list that were NOT requested: %s" % ", ".join(unexpected))
    if added:
        problems.append("titles appeared in the list: %s" % ", ".join(added))
    if still:
        problems.append("requested titles are still listed: %s" % ", ".join(still))
    report("ok" if not (unexpected or added or still) else "FAIL",
           "title list: removed=%s added=%s" % (", ".join(removed) or "none", ", ".join(added) or "none"))

old_hb = {entry["name"] for entry in before["homebrew"]}
new_hb = {entry["name"] for entry in after["homebrew"]}
gone, fresh = sorted(old_hb - new_hb), sorted(new_hb - old_hb)
if dry:
    if gone or fresh:
        problems.append("/data/homebrew changed during the dry run (gone %s, new %s)"
                        % (", ".join(gone) or "none", ", ".join(fresh) or "none"))
    report("ok" if not (gone or fresh) else "FAIL",
           "/data/homebrew unchanged (%d entr(ies))" % len(new_hb))
else:
    stray = [name for name in gone if name not in wanted]
    still = sorted(wanted - set(gone))
    if fresh:
        problems.append("/data/homebrew gained entries: %s" % ", ".join(fresh))
    if stray:
        problems.append("/data/homebrew lost entries that were NOT requested: %s" % ", ".join(stray))
    if still:
        problems.append("requested folders are still in /data/homebrew: %s" % ", ".join(still))
    report("ok" if not (fresh or stray or still) else "FAIL",
           "/data/homebrew: gone=%s new=%s" % (", ".join(gone) or "none", ", ".join(fresh) or "none"))

if want_data and dry:
    report("info", "%s would be removed too (--data)" % os.environ["TITLE_U_DATA_ROOT"])
if want_data and not dry:
    if after.get("data_dir_present"):
        problems.append("%s is still present after --data" % os.environ["TITLE_U_DATA_ROOT"])
    report("ok" if not after.get("data_dir_present") else "FAIL",
           "%s: %s" % (os.environ["TITLE_U_DATA_ROOT"],
                       "still present" if after.get("data_dir_present") else "absent"))

for title_id in ids:
    was_registered = title_id in old_titles
    was_homebrew = before["per_title"].get(title_id, {}).get("homebrew_dir", False)
    per = after["per_title"].get(title_id, {})
    if not was_registered and not was_homebrew:
        report("skip", "%s was already uninstalled before this run" % title_id)
        continue
    if per.get("homebrew_dir") and not dry:
        problems.append("/data/homebrew/%s is still there" % title_id)
    if dry:
        report("info", "%s: /system_ex/app/%s would be released by the unregister"
               % (title_id, title_id))
        if per.get("user_app"):
            report("info", "%s: /user/app/%s stays (system staging; never deleted here)"
                   % (title_id, title_id))
        if per.get("appmeta"):
            report("info", "%s: /user/appmeta/%s stays (system metadata; never deleted here)"
                   % (title_id, title_id))
        continue
    if files_only:
        report("todo", "%s: folder removed; delete its tile on the console (Options > Delete)"
               % title_id)
        continue
    report("ok", "%s: /system_ex/app/%s released by the uninstall call (the empty"
           " mount point may stay)" % (title_id, title_id))
    if per.get("user_app"):
        leftovers.append("/user/app/%s" % title_id)
    if per.get("appmeta"):
        leftovers.append("/user/appmeta/%s" % title_id)

for line in lines:
    print(line)
for note in leftovers:
    print("  left  %s is still there (the system's; this tool never deletes it)" % note)
print("")
if problems:
    print("title-uninstall: FAIL")
    for problem in problems:
        print("  %s" % problem)
    sys.exit(1)
if leftovers:
    print("title-uninstall: unregistered and removed; %d leftover director(ies) remain" % len(leftovers))
    for note in leftovers:
        print("  %s" % note)
    print("  remove them on the console if you want them gone too (this tool may not).")
    sys.exit(6)
if dry:
    print("title-uninstall: dry run OK — nothing was uploaded, unregistered or deleted")
else:
    print("title-uninstall: PASS — %s, nothing else changed"
          % ("folders removed, tiles left to delete" if files_only else "unregistered, removed"))
sys.exit(0)
PY
rc=$?
exit "$rc"
