# Network locations

A network location is a share on this LAN that the console reads as a library:
its scenes show up in the folder grid, its photos in the picker's tabs, and a
conversion whose album lives there is built locally and uploaded when it is
finished. Nothing is installed on the console and no service runs on a NAS: the
title speaks SMB2/SMB3 or NFSv3/NFSv4 itself.

`Settings > Network locations` is the one place they are configured.

> **Credentials are stored in plain text.** A location's password is written to
> `<state>/locations.json` on the console. Anyone with FTP or file access can
> read it, it is not encrypted at rest, and it is sent over the network in clear
> text unless SMB3 encryption is on. Use a dedicated account that can only read
> the one folder (and write it, if scenes are saved there).

## What a location is

| field | SMB | NFS |
| --- | --- | --- |
| Protocol | `SMB` | `NFS` |
| Host | **IP address** of the server (the console has no name resolver, see below) | same |
| Port | fixed port; `0` = default (445 SMB, 2049 NFS) | same |
| Share / Export | the share's name, e.g. `Photos` | the export path, e.g. `/export` |
| Path | folder inside the share; empty is its root | same |
| User | account name; empty = guest | not shown (AUTH_SYS) |
| Password | the account's password | not shown (there is no password) |
| Encrypt | SMB3 encryption on/off | not shown |
| UID / GID | n/a | the AUTH_SYS ids the server sees; `65534` = nobody |
| Version | n/a | `3` (default) or `4` |
| Photos tab | on/off: show a Photos tab for this share | on/off |
| Splat library | on/off: offer it as a library of scenes | on/off |

The NFS rows carry the note `NFS uses the console's user id; there is no
password` instead of the User/Password/Encrypt rows: see "NFS" below.

The Host row says *use the server's IP address*, and that is literal: the PS5
title's `getaddrinfo` is the payload SDK's stub (`ps5_getaddrinfo` returns
`EAI_FAIL` for every input, a dotted quad included), so nothing in the title can
look a name up. An address is parsed by the app itself and connects with no
lookup at all (`core/vfs/resolve.h`, `platform/ps5/net/resolve_glue.cpp`); a
host name fails before a packet is sent and the row says why:
`offline after 0.0 s: smb0: cannot resolve "nas.local": this build has no name
service (code 4); enter the server's IP address`. The first `viewer: net:` line
of a run states the same thing up front.

The `Path` field is what a library root becomes: a location with share
`Photos` and path `scenes` is opened as `smb0:/scenes`, and Cross on it in the
Locations screen pushes exactly that scene library. The id (`smb0`, `smb1`, …)
is assigned once, when the location is added, and is never reused: removing
`smb0` and adding another location gives `smb1`, so a saved path never starts
pointing at a different share. `nfs0`, `nfs1` follow the same rule.

A location can be marked as a **photo source** (the photos tab in the picker)
and as a **splat library** (the scene library); both are on/off rows on the edit
screen, so a scene-only share does not add an empty photo tab. The store keeps
the flags in `<state>/locations.json`.

The picker lists every location marked as a photo source as a tab, in store
order, and its last tab is **+ Add network location**: Cross opens the same
editor with an empty id and **Photos tab already On**, so the location appears
as a tab as soon as the editor returns. Adding a share therefore never needs
Settings, and nothing about locations is behind the visible-folders passcode any
more (stage U1). Turn the Photos tab row off in that editor for a share that is
only meant to be a scene library.

## Plain-text credentials

**The password is stored as plain text** in `<state>/locations.json`
(`/data/ps5-splat-vr/state/locations.json` on the dev install). The file is
created with mode `0600`, so only the console's own user can read it, but
anyone with that access - an FTP session, a backup of the data folder - can
read the password. The password row on the edit screen says so.

There is no key store in a PS5 title to encrypt it with: a key written next to
the file would protect it from nothing. The plan is to encrypt at rest once the
console offers a keystore the title may open; until then, give the share an
account that can only read (and, for uploads, write) that one folder.

## What works and what does not

Reads work everywhere the VFS is used: the folder grid, scene thumbnails, the
photo picker, scene loading (`UiCommand::OpenPath` reads a scene from the share
without copying it), and a conversion's source photo.

Folder **mutations** are refused on a location: create, rename and delete of an
album, and move, rename and delete of a scene answer

```
folders on a network location cannot be changed from the console
```

The same is true of the picker's "New folder" on a network library. That is a
deliberate limit for now: a second writer (a NAS, another console) makes the
conflict rules of a rename/move a problem of their own.

## The saved view of a scene on a share

A scene's 3D rotation is not a viewer setting: **L3/R3** on its file entry
rewrites the scene's own files (`scene.psplat`, `original.jpg`, `thumb.jpg` and
the intrinsics in `meta.json`) through the same VFS adapter as every other scene
file, so the turn travels with the scene and the next open — on this console or
another one — sees it. There is no `"view"."rotation"` to store any more.

For an **imported** scene, the scale the user settled on is stored in that
scene's own `meta.json` under `"view": {"scale": s}`, so it travels with the
scene too. On a location that is a normal write through the VFS; the next open
reads it back.

A **read-only** export cannot take that write. The scale store then keeps the
value in `<state>/views.json`, keyed by the location id and the path inside the
location (`smb0:photos/2026-10/scene`), so it survives on this console only. The
viewer logs it:

```
viewer: scale saved on this console only (scene switch: the scene folder took no write)
```

Nothing crashes and nothing is lost silently: the share keeps its own
`meta.json`, the console keeps the scale, and re-adding the location under the
same id finds it again. While the console entry exists it is what that console
reads — a scale set after the share turned read-only is the one shown, even
though the share's `meta.json` still carries an older `"view"` — and it is
removed as soon as the share takes a write again.

A turn of a scene on a read-only export is refused with the share's write error;
the source is left alone and the toast names it.

## Upload-then-delete

Converting a photo whose folder is on a location (`Albums`/`Scenes` opened from
the Locations screen, then `+` and Convert) does not write straight to the
share:

1. the scene is written locally, to `<dataDir>/tmp/<job id>/`
   (`original.jpg`, `thumb.jpg`, `scene.psplat`, `meta.json`);
2. `copy_tree` copies it to `<output root>/<album>/<scene folder>`;
3. the staging folder must hold at least one file (an empty one is refused, so
   "every file arrived" is never vacuously true), and every file's size is
   compared with the staging copy;
4. only then is the staging folder removed.

A share that drops mid-upload therefore never leaves a half scene where the
gallery scans (the scene folder is only valid once `meta.json` is there), and
the job fails with the reason and the copy's path:

```
upload failed: <reason>; kept at /data/ps5-splat-vr/tmp/job-000042
```

The kept folder is the finished scene; uploading it again by hand is enough to
recover, and nothing in the gallery lists it (it is not under `scenes/`).
A failed job's row in `Conversions` carries the same message.

## Error strings the screens show

| situation | message |
| --- | --- |
| no location with that id | `unknown location "smb3" for smb3:/scenes (add it in Settings > Network locations)` |
| a folder mutation on a share | `folders on a network location cannot be changed from the console` |
| a mount that does not answer | `offline: smb0: cannot connect to nas.local/Photos: <libsmb2 reason>` |
| a share whose path is missing | `offline: smb0: "scenes" does not exist on nas.local` |
| a save with no host or share | `a location needs a host` / `a location needs a share` |
| an upload that failed | `upload failed: <reason>; kept at <path>` |
| an NFS mount that does not answer | `offline: nfs0: cannot mount nas.local:/export: <libnfs reason>` |
| NFS support not built in | `offline: nfs0: NFS support is not built into this version` |
| an export path that does not exist | `offline: nfs0: "scenes" does not exist on nas.local` |

The mount probe is one root `stat` with a 5 s timeout (`smb2_set_timeout` for
SMB, `nfs_set_timeout(5000)` for NFS), so a switched-off NAS reports `offline`
in the Locations screen instead of hanging the panel. The root stat must also
answer "exists": a location whose share answers but whose `Path` is not there
is `offline` too, instead of `ready` with an empty grid. The screen's state
column is the last result: `ready`, `mounting`, or `offline: <reason>`.

**The adapter is registered before that probe runs**, not after it succeeds. The
two are different questions: registration answers "does `smb0:/…` route
somewhere", the probe answers "did the share answer". A host registers every
configured location at start-up (`LocationStore::register_adapters`) because the
job queue starts before the mount worker has answered, and a job resumed from
the last session uploads into its destination immediately - with registration
tied to the probe, that job reported `unknown location "smb0"` for a location
that was configured in the same boot. An adapter connects lazily, so
registration cannot block; while the probe is still running the row says
`mounting`, and an `offline` row's adapter is still registered (the error names
the location, it is not "unknown"), which is also what makes "retry on use" work
when the NAS comes back without a second mount.

## NFS

NFS is the other protocol, through libnfs 8.0.0 (`core/vfs/nfs.cpp`). It is a
different bargain from SMB, and the screen says so instead of hiding it:

- **There is no password.** NFS authenticates with **AUTH_SYS**: the client
  sends a `uid` and a `gid`, and the server decides what they may read or
  write from its own export rules and file ownership. That is the protocol, not
  a limit of the library, so the edit screen hides `User`, `Password` and
  `Encrypt` and draws `NFS uses the console's user id; there is no password`.
- **UID/GID default to 65534** (`nobody`/`nogroup`). A console that names no
  user must not act as root on the NAS. Type the uid/gid that owns the exported
  folder (on a typical NAS that is an account's number, e.g. `1000`), or keep
  the default and export the folder read-only.
- **Export paths.** `Export` is the path the server exports (`/export`,
  `/volume1/photos`, ...) and `Path` is a folder inside it; a location with
  export `/export` and path `scenes` is `nfs0:/scenes` in the VFS, exactly like
  the SMB share/path pair. The adapter mounts the export once and addresses
  everything below it by path, so a relative `Path` costs no second mount.
- **Version.** `Version 3` (the default) uses the MOUNT protocol and the
  traditional export model; `Version 4` is NFSv4.0 (libnfs's `NFS_V4`: it has
  its own single port and no MOUNT). 4.1's sessions and 4.2's server-side copy
  are a different libnfs dialect and are not selected here - see
  `docs/BACKLOG.md`. v3 is what a Synology/QNAP/TrueNAS
  "NFS v3" export and a Linux `exports` line give; v4 is what a server with
  only `/etc/exports` v4 pseudo-roots gives. There is one shared path grammar
  because a PS5 needs no mount table: this is a user-space client, not a
  kernel mount.

### Ports, rpcbind and what a NAS needs

The adapter talks to fixed ports when the location's `Port` is set: the URL it
builds and logs is

```
nfs://<host>/<export>[/<path>]?version=3&nfsport=<p>&mountport=<p>&uid=<u>&gid=<g>
```

(`version=4` drops `mountport`: NFSv4 has no MOUNT program). `Port` is a row
on the edit screen; `0` (drawn as `Default`) leaves both arguments out and
libnfs uses the defaults and rpcbind for the MOUNT port, which is the normal
NAS configuration. A server with a fixed port - the test server below, or a
NAS that does not run rpcbind - is reached by typing that number in `Port`.
Nothing here uses NLM locking, rpcbind discovery, NFSv2 or Kerberos.

What a NAS needs for the client to work:

- an export the client's uid/gid may read (and write, for an upload target);
- for an NFSv3 export, the `insecure` option. libnfs tries to source its
  connections from a privileged port (512-1023) and, when `bind()` answers
  `EACCES`, stops trying and connects from an ephemeral port instead
  (`third_party/libnfs/lib/socket.c`: the privileged-port `bind()` loop in
  `rpc_connect_sockaddr_async()` breaks out on `EACCES` and never retries). A
  console title is not root, so the
  ephemeral path is the normal one, and a Linux `exports` line with the default
  `secure` refuses that source port - the mount fails with `MNT3ERR_ACCES`
  (mountd) or the listing fails with `NFS3ERR_ACCES`. Add `insecure` to the
  export (and to `mountd`, which checks the same thing) for NFSv3; NFSv4 has
  no separate MOUNT program but the server still checks the source port of the
  nfsd connection, so `insecure` is the safe answer there too. The local
  `nfsd.py` test server does not check the source port, which is why the live
  check below passes without it;
- squash settings to taste: a NAS that squashes to `nobody` will make every
  write appear as `nobody`, which is why the default uid/gid here is 65534 as
  well - the two then agree instead of fighting.

### Testing against a local NFS server

`runs/v0.2-library/ref/nfsd` (anyvm-org/nfsd) is a single-file, stdlib-only,
user-space NFSv3/v4 server: no root, no Docker, no rpcbind, no kernel module.
It serves the MOUNT program on the same port as NFSv3, which is why the URL
carries `nfsport` and `mountport`:

```sh
D=/path/to/runs/v0.2-library
git clone --depth 1 https://github.com/anyvm-org/nfsd $D/ref/nfsd
mkdir -p $D/nfs-export/scenes
python3 $D/ref/nfsd/nfsd.py -dir $D/nfs-export -port 2049 -vers 3 -v
```

`nfsd.py` exports `-dir` as the export **`/`** (a mount of `/scenes` answers
`MNT3ERR_NOENT`), so the app's `Export` is `/` for it - in the URL form
`nfs://127.0.0.1?...`, with no path after the host. In the app: Protocol `NFS`,
Host `127.0.0.1`, Export `/`, Path empty (or `scenes`), Version 3, Port `2049`
(or `0`/`Default` - 2049 is the default anyway), UID/GID the ids that own the
folder. Then the automated live check:

```sh
SPLAT_NFS_URL='nfs://127.0.0.1?version=3&nfsport=2049&mountport=2049&uid=501&gid=20' \
    ./build/tests/splat_tests -tc='*nfs-live*'
# v4 works against the same server with -vers 4, no mountport:
SPLAT_NFS_URL='nfs://127.0.0.1?version=4&nfsport=2049&uid=501&gid=20' \
    ./build/tests/splat_tests -tc='*nfs-live*'
```

It lists the export root, writes `splat-vfs-live.txt`, reads it back, stats it,
unlinks it and checks it is gone. NFS writes fail with
`NFS3ERR_ACCES` when the uid/gid the URL names does not own the folder - that
is the server's answer, shown verbatim in the error. The unit tests
(`tests/vfs_nfs_test.cpp`) need no server: the URL shapes for v3/v4 and the
default ports, and a mount of `127.0.0.1:1` that must fail inside the timeout
with `nfs0` in the message.

### NFS error strings

| situation | message |
| --- | --- |
| mount refused / closed port | `nfs0: cannot mount 127.0.0.1:/export: <libnfs reason>` |
| listing refused | `nfs0: cannot list scenes: <libnfs reason>` |
| read refused | `nfs0: cannot read scene.psplat: <libnfs reason>` |
| write refused | `nfs0: cannot create out/scene.psplat: <libnfs reason>` |
| unlink refused | `nfs0: cannot remove scene.psplat: <libnfs reason>` |
| no host or export | `nfs0: an NFS location needs a host and an export` |

Every message names the location id and carries libnfs's own reason
(`nfs_get_error`), so a NAS that refuses a mount says why (`MNT3ERR_ACCES`,
`RPC error: Mount failed ...`, a timeout) instead of "offline".

## SMB3 encryption

`Encrypt` sets `smb2_set_seal(ctx, 1)`, which is AES-128-CCM and requires
SMB 3.x; the adapter also pins the dialect to SMB3 (`SMB2_VERSION_ANY3`) so a
server that negotiates 2.1 fails with a clear reason instead of silently going
unencrypted. It is **not tested end to end**: the test server below (impacket's
`smbserver`) does not implement SMB3 encryption, and the console check is in
`CONSOLE-TEST.md`. Signing and NTLMv2 are always on; Kerberos is not built.

SMB1 is not supported (libsmb2 6.x is SMB2/3 only), and neither is browsing the
server's share list: the share name is typed, which is also what a user knows
about their own NAS.

## Removing a location

`Settings > Network locations`, focus the share, Square, then `Delete` on the
card. That removes it from `locations.json` and unmounts it; **the files on the
share are not touched**. A library screen that was opened from it before will
fail its next listing with `unknown location "<id>"` - go back and open another
library.

## Testing against a local SMB server

`runs/v0.2-library/` has the recipe (Appendix C of the spec): impacket's
`smbserver` in a venv, no admin and no Docker. Port 445 is privileged on macOS,
so it listens on 1445 - which the location form accepts because the adapter
passes `host:port` to libsmb2:

```sh
D=/path/to/runs/v0.2-library
python3 -m venv $D/venv-smb
$D/venv-smb/bin/pip install 'impacket==0.11.0'
mkdir -p $D/smb-share/photos
$D/venv-smb/bin/python $D/venv-smb/bin/smbserver.py -smb2support \
    -username test -password test -port 1445 -ip 127.0.0.1 \
    PHOTOS $D/smb-share
```

The first positional argument is the share name and the second the folder:
impacket 0.11's usage line is `smbserver.py [...] shareName sharePath`, and
swapping them makes every tree connect answer "not found" (libsmb2 then reports
`Failed to parse fixed part of command payload`, which looks like a protocol
bug and is not one).

In the app: Protocol `SMB`, Host `127.0.0.1`, Port `1445`, Share `PHOTOS`,
Path `photos`, User `test`, Password `test`, Encrypt off. Then the automated
live check:

```sh
SPLAT_SMB_URL='smb://127.0.0.1:1445/PHOTOS/photos?user=test&password=test' \
    ./build/tests/splat_tests -tc='*smb-live*'
```

It lists the share root, writes `splat-vfs-live.txt`, reads it back, stats it
and unlinks it.

## Visible folders

A location that is a **splat library** is one row of the Visible folders
allow-list ([docs/visible-folders.md](visible-folders.md)): ticking it publishes
the whole library, its scenes and every subfolder below its `path`. Two families
on one share need two locations (the same host and share with different paths).
A location that is only a photo source has no row there, because the photo
picker is not filtered.

This screen stays fully manageable with a passcode set (stage U1, user decision
2026-11-06): Settings → Network locations lists every location, with the Add row,
Triangle (edit) and Square (delete), and the editor saves. The passcode hides a
ticked location's scenes, not its configuration. The consequence is recorded in
[docs/visible-folders.md](visible-folders.md#residual-limit-a-ticked-location-can-be-edited):
a ticked location can be edited to point elsewhere and keeps its tick. The unit
tests (`tests/vfs_smb_test.cpp`, `tests/vfs_location_test.cpp`) run without a
server; the offline case connects to `127.0.0.1:1` and only checks that the
failure names the location and never the password.
