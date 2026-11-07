# Changelog

## 0.6.0

### Added

- TV menu: in VR the TV now shows the scene without the VR window or HUD and, while the menu is open, the same menu flat on the whole screen. Anyone holding a controller can drive it. It starts with the app, so the person at the TV can pick a scene before the headset is on (a "Waiting for the headset" banner shows meanwhile). If the console refuses the TV output, or it stops working, the TV shows the headset's view as in 0.5.x. `TV_OUTPUT=0` in `viewer.env` turns it off.
- Output folder: Settings > Output folder chooses where new scenes are saved, the console (default) or a folder on a network location, picked by browsing that location's folders (Square = "Use this folder"). Conversions and splat imports started from the console's shelf go to `<output folder>/<album>/<scene>`, so the albums look the same as on the console; an import saves a converted copy. Choosing a folder switches that location's scene library on. Existing scenes stay where they are.
- Network scene libraries show albums like the console does: Settings > Network locations, Cross on a location, lists the folders that hold scenes as albums, and the scenes directly in the folder as Unsorted. An offline share shows its own error. Folders on a share still cannot be created, renamed or deleted from the console. The location that holds the Output folder has an extra row for it.
- A scene whose network folder does not answer when it is ready no longer fails. It shows "Waiting for <location>", keeps the finished scene on the console, tries again by itself (every few seconds at first, later every 2 minutes, and when the app starts) and saves it when the share is back. Square on the row cancels it. A share that answers but refuses the write, or a location that was removed, still fails.
- Model download: the SHARP checkpoint (2.8 GB) downloads over four connections at once, about 3 times faster on the console, and the Models row shows "about N min left". A paused or interrupted download continues where it stopped, and the checkpoint is still verified after it arrives.

### Fixed

- The scene and the menu are in front of you at launch. The headset's tracker starts about 90 degrees off and corrects itself a few seconds later, which left the scene to your right; the app now notices the correction and puts the scene and the open menu in front of where you face.
- Hold Options for 1 second to recentre the scene and the open menu, like the PSVR2 standard. A tap on Options still opens and closes the menu, now when you let go. Hold Cross keeps its meaning. A black view always has a way out.
- VR menu window: moving or turning your head no longer skews its contents. The window stays a rigid object fixed in the world.
- Photo picker: a network location is browsed folder by folder instead of one flat list of every photo on the share.
- A share that drops while a scene is being saved no longer leaves a half-saved scene or a duplicate behind: the scene is saved under a hidden temporary name and only gets its real name once it is whole.

### Changed

- Photo picker: every source (Console, Inbox, USB, network locations) is a folder browser like Finder or Windows Explorer. Folders come first, then the photos and splat files of the folder you are in; Cross opens a folder, Circle goes up. Square on a folder no longer imports every splat file below it: you pick the files yourself, and Cross on a splat file imports it, from any source.
- The version is 0.6.0 (`contentVersion` `01.006.000`).

## 0.5.0 (2026-11-06)

- Photo formats: WebP, TIFF, HEIC/HEIF, AVIF, BMP, GIF (first frame), TGA and camera RAW (DNG, CR2, CR3, NEF, ARW, RAF, ORF, RW2, PEF, SRW).
- EXIF orientation (all eight values) turns the pixels and the intrinsics; thumbnails and focal length follow.
- Rotate left or right (L3/R3) on any photo or scene entry; the rotated version overrides the previous one.
- Rotation removed from the viewer: a scene is turned on its file entry (L3/R3), not with the D-pad or Square.
- Splat import: `.ply`, `.splat`, `.spz` (v1-v4) and SuperSplat `.compressed.ply`, thinned to the renderer's 2,000,000-splat budget.
- Network libraries: SMB2/SMB3 and NFSv3/NFSv4 locations for photos, scenes or both.
- Upload-then-delete: a scene saved to a network location is built in a local staging folder, uploaded and removed; a failed upload keeps the staging copy and names it.
- Visible folders: one passcode hides the scene albums and network scene libraries that are not ticked; the photo picker stays open (stage U1).
- Thumbnail size: one Settings slider sizes every grid, with one shared tile shape and label rule for folders, scenes and photos.
- Scaling: the default pivot is the photo's Capture point (L2/R2 zoom about where the photo was taken); a stored Front/Centre value is kept.
- The version is 0.5.0 (`contentVersion` `01.005.000`).
