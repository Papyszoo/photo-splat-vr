# Visible folders

One passcode protects one allow-list of **scene folders**. With the passcode
set, only the folders ticked in **Settings → Visible folders** exist on the
shelf: their albums, scenes, thumbnails, jobs and remembered scene are gone from
every screen, in VR and on the TV, until the owner ticks them. Everything below
a tick is published by that tick.

> This hides scene folders in this app and does not encrypt them: anyone with
> the share, FTP or another app can still open the files.

The app-UI allow-list is the whole protection. It is not a lock on the files and
it is not a second account.

## What it does not hide: photos

The photo picker is **not filtered** by this list. Whoever holds the controller
can open a folder's `+` tile → **Photos** and browse every source: the Inbox,
the console's storage, USB sticks, captures and every network location that is a
photo source, including every photo in them. That is the user's decision of
2026-11-06: *"Hiding folders was only for scenes."* Hiding photos is not what
this passcode does; use the console's own user/profile controls or a separate
account on the NAS if photos must be protected.

Network locations are not hidden either: **Settings → Network locations** lists
and manages every configured share with or without a passcode, and so does the
picker's location tabs and its **+ Add network location** tab. A ticked location
hides its **scenes** on the shelf, not the share's configuration or its photos.

## Recovery

The passcode is not stored, only a salted hash. There is no "forgot passcode"
screen and no reset code. To recover, delete this file from a PC or FTP client:

```
/data/ps5-splat-vr/state/visible-folders.json
```

The next launch has no passcode and every scene folder is visible again. The
same delete is the bypass for anyone with file access — that is what "not
encryption" means.

## What a folder is

A row of the tick list is where scenes are listed. There are four kinds:

| kind | one row per | label on the screen | what the tick makes visible |
| --- | --- | --- | --- |
| `album:<name>` | a top-level album folder under the scene library | the album name | that album, its scenes, their thumbnails, convert/move destinations |
| `unsorted` | the loose scenes directly under the library | "Unsorted" | that implicit album |
| `other` | scenes with no trustworthy folder | "Other scenes" | those scenes, inside whatever album ticks are on |
| `location:<id>` | a configured SMB/NFS location that is a **splat library** | its display name | that location's scene library row and every subfolder of it |

The screen is grouped **Scene folders** (Unsorted + every album), **Network**
(every location that is a splat library) and **Other** (Other scenes), followed
by **Change passcode** and **Turn off passcode**.

A location that is only a photo source has no row: it is not part of this list.
Choosing an Output folder on a location (Settings > Output folder) switches its
splat library on, so it gets a row here like any other; until the owner ticks it,
a conversion into it is refused with "That folder is not visible".

Rules that follow from the table:

- **The list is top-level only.** Ticking a folder publishes everything below it.
  There is no deeper tick and no per-file rule. A tick is a trust boundary, and
  the owner holds a controller, not a file manager.
- **A network location is one tick.** The configured `path` is the boundary. Two
  families on one share need two locations (the same host and share with
  different paths is fine). The app never enumerates an offline NAS to build a
  tree, and a location id is stable.
- **The list always has "Unsorted" and "Other scenes",** even when empty.
- **Something that appears later is unticked and hidden.** A new album or a new
  location is listed (once the passcode is entered) and stays hidden until the
  owner ticks it.
- **Photo folders, USB sticks, captures and console folders are not rows.** They
  were in the stage-H list; stage U1 removed them, because the picker is open
  again (see above).

The README tells the owner to put each family in its own top-level folder or its
own location; that is the only rule the app depends on.

## How a scene is tied to its folder

A scene is visible only when its **placement** is ticked: the album
(`album:` / `unsorted`), the "other" group or the network location
(`location:`) it is listed under. Nothing else decides.

- `meta.json` is **not read** for this. `meta.source_folder` and the `origin`
  object decide nothing (stage U1 dropped the old "placement AND source" rule).
  A scene converted from a photo folder that is no longer tickable stays visible
  in a ticked album.
- A scene whose `meta.json` is missing or unreadable is still judged by the
  album or location it sits in; an unticked placement hides it exactly as any
  other scene.
- A scene in a folder with no row of its own (two levels below the library root)
  is "other" and needs **Other scenes** ticked, as before.

New conversions and imports still write `meta.source_folder` (the folder the
photo or splat file came from) and `meta.source` as before. That field is now
history for the owner's benefit, not a rule: it is recorded and never used to
hide a scene.

Old state files that still hold the photo keys of stage H (`source:inbox`,
`folder:/data/Photos`, …) load normally, and those keys are inert: no row, no
photo filter, and not a damaged file. Nothing has to be deleted after the
update.

## Setting, changing and removing it

Settings → Visible folders has four states:

- **No passcode** — says so, with the warning, and offers "Set a passcode".
- **Locked** — a passcode is set and this visit has not entered it yet.
- **Damaged** — the state file exists but cannot be understood. Nothing is
  visible; only deleting the file from a PC recovers it.
- **List** — unlocked: "Scene folders" (Unsorted + albums), "Network" (scene
  locations), "Other scenes", then "Change passcode" and "Turn off passcode".

The passcode is typed on the panel keyboard, masked (one `*` per character),
between 4 and 32 characters, with no complexity rule: the owner types it with a
controller. Setting it starts from nothing ticked, so the owner chooses what may
be seen instead of handing over the whole library. **Set the passcode before the
visitor arrives.**

A tick is saved the moment it is made. The row's right-hand text is "Visible" or
"Hidden". "Turn off passcode" asks first and then deletes the state file; every
scene folder becomes visible again.

**The list is open only while the screen is open.** Circle locks it and returns
to Settings. Options closes Settings too. The passcode is asked again on every
new open: there is no session unlock, no "unlocked" flag on disk and no timeout,
because a visitor who picks up the controller after the owner walks away must
not find the list open.

A scene that is open in the viewer when it becomes hidden is closed (the panel
stays up, the picture does not), and a remembered scene is not resumed.

## The passcode file

`/data/ps5-splat-vr/state/visible-folders.json`, mode 0600, never in
`locations.json`, never on a share:

```json
{
  "format_version": 1,
  "passcode": {
    "algorithm": "pbkdf2-hmac-sha256",
    "iterations": 120000,
    "salt": "<32 lowercase hex>",
    "hash": "<64 lowercase hex>"
  },
  "visible": ["album:Family A", "location:smb0", "other"]
}
```

- The passcode itself is never written, logged or drawn. Only the PBKDF2-HMAC-
  SHA256 output over a fresh 16-byte salt is stored, and a change always makes a
  new salt.
- 120000 iterations take about 0.1 s on the development Mac and are computed on
  the UI thread when the owner submits; the unlock pauses for that.
- A missing file is "no passcode, every scene folder visible". A file that exists
  and cannot be understood — malformed JSON, a version above 1, no passcode
  object, an unknown algorithm, an iteration count outside 1000…1000000, a salt
  or hash of the wrong length, an unparseable key — is **fail closed**: nothing
  is visible and the screen says the file is damaged. A typo never shows the
  shelf.
- There is no lockout and no retry limit. A wrong passcode says "Wrong passcode"
  and asks again.

## What is protected and what is not

Protected: the scene shelf. Album and scene listings, the scene gallery,
thumbnails, imported scenes, network scene libraries, the conversion queue and
its messages, toasts, the last opened or remembered scene, and opening a scene
by a remembered path. The Settings row says only "Off" or "Protected" — never a
folder name or a count.

Not protected (accepted, by design):

- **The photo picker and every photo.** It is not filtered: a visitor can browse
  the Inbox, the console's folders, USB sticks, captures and every photo on every
  location. This is the user's 2026-11-06 decision, not an oversight.
- **Network location management.** Settings → Locations and the picker's
  "+ Add network location" tab add, edit, test and remove shares with or
  without a passcode (a share added from the picker starts as a photo source,
  so its tab appears at once). A visitor can add a share; the passcode only
  hides the scenes a ticked location holds.
- **The files.** FTP, SMB, another app on the console and a PC can read, copy or
  delete them. The allow-list is a UI rule.
- **The state file.** Anyone with file access can read it (it holds keys and a
  hash, not the passcode) or delete it — which is the documented recovery.
- **Name probing.** A visitor who guesses a hidden folder's exact name can learn
  that the name is taken by trying to create or rename a folder to it. The
  contents stay hidden. Closing that would mean forbidding create/rename while a
  passcode is set.
- **Mutations.** Whoever holds the controller can still create, rename, delete,
  move, rotate and convert inside what is visible.
- **Stable identities.** Album names are not stable ids: an album renamed away
  and a later folder that takes the old name inherits the old tick. Network
  location ids are never reused.
- **One local passcode, one owner.** Anyone using the app can set a passcode when
  none is set.

### Residual limit: a ticked location can be edited

A ticked scene location can still be edited in Settings → Network locations (or
from the picker's add tab) to point at another host, share or path, and it keeps
its id and its tick. Stage H avoided that with a read-only mode; the user's
2026-11-06 decision removed it, because managing locations belongs in Settings.
A visitor who reaches Settings can therefore re-point a ticked library at a share
they can read. There is no automatic un-tick (no extra UI): after editing a
location, open Visible folders and check its tick if that matters.

See also: [Network locations](network-locations.md), the README's "Visible
folders" paragraph, and `CONSOLE-TEST.md` A7 for the console checks.
