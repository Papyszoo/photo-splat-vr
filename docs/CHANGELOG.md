# Changelog

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
