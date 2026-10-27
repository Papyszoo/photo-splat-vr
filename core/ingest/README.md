# core/ingest — LAN photo upload (C9)

A phone opens `http://<console-lan-ip>:8090/`, enters the six-digit PIN shown on
the TV, picks photos from the camera roll and watches them land in the queue.

## Network exposure

The server binds **one private LAN address** (`10/8`, `172.16/12` or
`192.168/16`), detected at startup. It never binds `0.0.0.0` and never uses
UPnP, NAT-PMP or port forwarding, so a router cannot expose it to the internet
by accident. An explicit bind address must be private IPv4 or `127.0.0.1`;
anything else is refused. When no private address exists it falls back to
`127.0.0.1`, which only the console itself can reach.

Plain HTTP on the LAN is intentional: there is no certificate the phone would
trust for a made-up host, and the payload never leaves the local network. The
session cookie is `HttpOnly` and `SameSite=Lax`; it is not `Secure` because the
page is served over HTTP.

## Files

- `pin_gate.{h,cpp}` — six-digit PIN and 128-bit session tokens from the system
  CSPRNG (`arc4random_buf` on macOS/FreeBSD and the PS5 payload SDK,
  `getrandom` with `/dev/urandom` on Linux), constant-time token and PIN
  compare, per-address lockout (5 wrong tries → 60 s, configurable) and a
  global cap (20 wrong PINs in a rolling minute → everyone locked out for
  60 s).
- `upload_store.{h,cpp}` — JPEG/PNG magic-byte sniffing, size limit (default
  40 MB), safe names, and atomic publish into the inbox (`write` → `fsync` →
  `rename`; temp name is `.upload-*.part`, which the watcher ignores).
- `ingest_server.{h,cpp}` — cpp-httplib routes and the embedded page.
- `ingest_page.h` — the single self-contained HTML page (no external assets).
- `lan_address.{h,cpp}` — private-LAN IPv4 detection via POSIX `getifaddrs`.

Uploads are **not** enqueued here. They land in the inbox directory; the
existing `core/jobs` `InboxWatcher` and `JobQueue` pick them up, exactly as for
a USB drive or FTP copy. `QueueProvider` is a read-only snapshot callback, so
the HTTP layer never owns or mutates queue state.

## API

| Route | Auth | Body | Result |
| --- | --- | --- | --- |
| `GET /` | no | — | embedded page |
| `POST /api/login` | PIN | `pin=123456` (form) | `200` + session cookie, `401` wrong PIN, `429` locked out with `Retry-After` |
| `POST /api/upload` | cookie | multipart field `photo` | `200 {"ok":true,"file":"…"}`, `400` bad content/name, `413` too large |
| `GET /api/queue` | cookie | — | `{"ok":true,"jobs":[…],"counts":{…}}`, `401` without a session |

`GET /api/queue` returns file names only, not inbox paths.

Wrong PINs are limited twice: five per client address, and twenty across all
clients in a rolling minute. The second cap is what stops many devices from
multiplying the first; while it is on, a correct PIN waits too (a deliberate
trade: a minute of denial instead of an open six-digit guessing oracle).

## Manual testing

```sh
./build/tools/ingest_server/splat_ingest_server \
    --port 8090 --inbox /tmp/inbox --state /tmp/jobs.json --bind 127.0.0.1
```

It prints the PIN and serves until Ctrl-C. `--state` opts into the real
`InboxWatcher` + `JobQueue` loop, so an upload appears in `/api/queue` (the app
supplies its own queue snapshot instead). Omit `--bind` to use the detected LAN
address.

## Tests

`tests/ingest_pin_test.cpp`, `tests/ingest_upload_test.cpp` and
`tests/ingest_server_test.cpp` cover the I/O-free PIN logic (per-address and
global lockout, rolling window, token format/uniqueness), accepted and rejected
uploads (bad magic, oversize, traversal names), atomic landing plus watcher
pickup, and the queue JSON against a real `JobQueue`.

## Portable build

The implementation uses POSIX sockets and threads only (cpp-httplib's POSIX
path), `std::filesystem`, and no macOS APIs, so the same sources can be built
into the PS5 payload later. `tools/ingest_server` is the desktop CLI.
