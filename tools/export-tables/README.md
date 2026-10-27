# tools/export-tables — PS VR2 library export offsets

The PS VR2 system libraries (`libSceHmd2`, `libSceVrTracker2`, `libSceVrHand`)
keep the functions C7 needs out of their dynamic symbol tables, so there is
nothing to `sceKernelDlsym`. The app calls them at fixed offsets inside one
*build* of one firmware instead. This directory holds those offsets and the
generator that turns them into the C header `platform/ps5/hmd2/hmd2.cpp`
includes.

```
tables.json                      the data: per library, per firmware, offsets + anchor
gen_export_table.py              tables.json -> platform/ps5/hmd2/hmd2_exports.h
gen_export_table.py --sprx ...   a decrypted library -> a table entry, or --verify
```

`hmd2_exports.h` is generated and committed: the build needs no decrypted
libraries, only the JSON. Regenerate after any change to `tables.json`:

```sh
python3 tools/export-tables/gen_export_table.py --emit-c -o platform/ps5/hmd2/hmd2_exports.h
```

## How PS5VR does it, and what this copies

PS5VR (`tools/gen_export_table.py`, `app/src/vr_exports_*.h`, GPL-3.0) reads a
decrypted library, matches exports by name, and emits `{name, offset}` rows
plus one *anchor*: an address and a printable string that must sit at that
address in the library's read-only data. At runtime the app checks the anchor
against the loaded library and only then uses the offsets, so a table from
another firmware build cannot be applied by accident — it is skipped, and the
app reports "no export table for this firmware".

This copy keeps that scheme and drops the dependency on PS5VR's 646-line
`ps5_dynlib.py`: the NID of a name is a SHA-1, so the lookup needs no name
database at all. The anchor selection (first printable string of 12+ characters
in the read-only `PT_LOAD`, regex included) is PS5VR's, unchanged; anchors were
copied from its generated headers, so keep the two in step if either changes.

## The data

| Library | Firmware | Serves | Anchor |
| --- | --- | --- | --- |
| `libSceHmd2` | 1360 | 13.20, 13.60 | `Not Initialized` @ `0x7c686` |
| `libSceHmd2` | 761 | 7.20, 7.61 | `Awarped depth mip chain R` @ `0x1306c3` |
| `libSceVrTracker2` | 1360 | 13.20, 13.40, 13.60 | `MININLIERS-FOR-MULTICAMSTRICTGRAVITYBOOT` @ `0x1225e5` |
| `libSceVrTracker2` | 761 | 7.20, 7.61 | `SceVrtMtxDepthMapWork` @ `0x116779` |
| `libSceVrHand` | 1360 | 12.20 and later | `handPose_%02d` @ `0xb84e0` |

Only the exports C7 calls are listed; PS5VR's tables carry more (mirroring, eye
tracking, vibration, the camera opens). Add a name to `tables.json` and
regenerate when the library needs it — prefer names that both firmware tables
have, or the 7.x entry needs a "not in this firmware" path.

7.20 has no table of its own because PS5VR has none: its 761 table was
generated from 7.61's libraries and documented as identical to 7.20's
(`PS5VR/app/src/vr_exports.h`, `docs/firmware-support.md`). On a 7.20 console
the anchor check decides.

## Adding a firmware

1. Four decrypted libraries from a console on that firmware (they live in
   `/system/common/lib/` and are encrypted on disk; `ps5-self-pager` output is
   what this script reads): `libSceHmd2.sprx`, `libSceVrTracker2.sprx`,
   `libSceVrHand.sprx`, optionally `libSceSystemService.sprx`.
2. Read one and compare it with the table:

   ```sh
   # prints the offsets + anchor this library actually has
   python3 tools/export-tables/gen_export_table.py \
       --sprx libSceHmd2.sprx --lib libSceHmd2 --fw 1240 \
       --names sceHmd2Initialize,sceHmd2Open,... > hmd2-1240.json

   # or check an existing table against the library (exit 1 on any difference)
   python3 tools/export-tables/gen_export_table.py \
       --sprx libSceHmd2.sprx --lib libSceHmd2 --verify
   ```

3. Paste the printed block into `tables.json` under `libraries.<lib>.firmware.<fw>`
   (with a `covers` note), add the offset of the anchor it prints, regenerate the
   header, rebuild.

`--names` defaults to the names already listed for that (library, firmware), so
adding a firmware means listing them once.

Verified here without a console: `--emit-c` round-trips `tables.json` to the
committed header, and the NID hash self-check matches PS5VR's assertion
(`sceHmd2Initialize` → `c812oYs7Vsc`). `--sprx` needs decrypted libraries, which
this machine does not have: it is untested, and the first console-side firmware
table will be its first real run.
