#!/usr/bin/env python3
"""Per-firmware export tables for the PS VR2 system libraries.

Two jobs, one script, the same idea as PS5VR's tools/gen_export_table.py:

  --emit-c      tables.json -> platform/ps5/hmd2/hmd2_exports.h (the build's
                only input; no decrypted libraries needed to build).
  --sprx        a decrypted library (ps5-self-pager output) -> the JSON block
                for it. With --verify, compare it against tables.json instead
                of printing and exit 1 on any difference.

The libraries (libSceHmd2, libSceVrTracker2, libSceVrHand) keep these functions
out of their dynamic symbol tables, so there is nothing to sceKernelDlsym: the
app calls them at fixed offsets inside one build of one firmware, checked by an
anchor string at a known address (see hmd2.cpp and docs/research/spike-b-hmd2.md).

usage:
  gen_export_table.py --emit-c [-o FILE]
  gen_export_table.py --sprx libSceHmd2.sprx --lib libSceHmd2 [--fw 1360]
                      [--names N1,N2,...] [--verify]

Offsets come from the library itself: the wanted names hash to NIDs (SHA-1 of
the name plus a fixed suffix), so no name database is needed. --fw defaults to
the newest firmware listed for --lib in tables.json; --names defaults to the
names listed there for that (library, firmware), and is required for a
firmware that has no table yet.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import base64
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TABLES = HERE / "tables.json"
OUT_C = HERE.parent.parent / "platform/ps5/hmd2/hmd2_exports.h"

# The PS5/PS4 import-ID suffix: name -> NID (11 base64 chars) is SHA-1 of the
# name plus this constant; the first 8 digest bytes, read as a little-endian
# u64 and base64'd big-endian, are the NID.
NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_GNU_HASH = 0, 4, 5, 6, 10, 0x6FFFFEF5


def nid_of(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(digest[7::-1], b"+-").rstrip(b"=").decode()


class Sprx:
    """A decrypted PS5 library: ELF64 with a Sony-style dynamic section."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2:
            sys.exit(f"{self.path}: not an ELF64 file (decrypt with ps5-self-pager first)")
        e_phoff, = struct.unpack_from("<Q", self.data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.data, 0x36)
        self.loads, dynamic = [], None
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIQQQQ", self.data, off)
            if p_type == PT_LOAD:
                self.loads.append(dict(flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                                       filesz=p_filesz))
            elif p_type == PT_DYNAMIC:
                dynamic = (p_offset, p_filesz)
        if dynamic is None:
            sys.exit(f"{self.path}: no PT_DYNAMIC (not a Sony dynamic library?)")
        self.dyn = {}
        for i in range(dynamic[1] // 16):
            tag, val = struct.unpack_from("<qQ", self.data, dynamic[0] + i * 16)
            if tag == DT_NULL:
                break
            self.dyn.setdefault(tag, val)

    def offset_of(self, vaddr):
        for seg in self.loads:
            if seg["vaddr"] <= vaddr < seg["vaddr"] + seg["filesz"]:
                return seg["offset"] + (vaddr - seg["vaddr"])
        sys.exit(f"{self.path}: address 0x{vaddr:x} is not in a loadable segment")

    def _symbol_count(self):
        if DT_HASH in self.dyn:
            off = self.offset_of(self.dyn[DT_HASH])
            return struct.unpack_from("<II", self.data, off)[1]     # nchain
        if DT_GNU_HASH in self.dyn:
            off = self.offset_of(self.dyn[DT_GNU_HASH])
            nbuckets, symoffset, bloom_size, _ = struct.unpack_from("<IIII", self.data, off)
            buckets = off + 16
            chains = buckets + 4 * nbuckets + 8 * bloom_size
            last = 0
            for i in range(nbuckets):
                bucket, = struct.unpack_from("<I", self.data, buckets + 4 * i)
                last = max(last, bucket)
            if last < symoffset:
                return symoffset
            while True:
                word, = struct.unpack_from("<I", self.data, chains + 4 * (last - symoffset))
                last += 1
                if word & 1:
                    return last
        sys.exit(f"{self.path}: neither DT_HASH nor DT_GNU_HASH: cannot size the symbol table")

    def exports(self):
        """{name or NID text: value} for every defined dynamic symbol."""
        strtab_off = self.offset_of(self.dyn[DT_STRTAB])
        strtab_end = strtab_off + self.dyn.get(DT_STRSZ, 0)
        symtab_off = self.offset_of(self.dyn[DT_SYMTAB])
        out = {}
        for i in range(1, self._symbol_count()):
            name_off, _, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", self.data, symtab_off + i * 24)
            if shndx == 0 or value == 0:
                continue                                    # undefined: an import
            end = self.data.find(b"\0", strtab_off + name_off, strtab_end + 1)
            if end < 0:
                continue
            out[self.data[strtab_off + name_off:end].decode("latin-1")] = value
        return out

    def anchor(self):
        """First printable C string of 12+ chars in the read-only segment."""
        seg = next((s for s in self.loads if s["flags"] == 4), None)
        if seg is None:
            sys.exit(f"{self.path}: no read-only PT_LOAD segment for the anchor")
        data = self.data[seg["offset"]:seg["offset"] + seg["filesz"]]
        match = re.search(rb"[A-Za-z][A-Za-z0-9 _%:.,()-]{11,59}\x00", data)
        if match is None:
            sys.exit(f"{self.path}: no anchor string found in the read-only segment")
        return seg["vaddr"] + match.start(), match.group(0)[:-1].decode()


def fw_name(fw):
    """1360 -> '13.60', 761 -> '7.61'."""
    padded = fw.zfill(4)
    return f"{int(padded[:-2])}.{padded[-2:]}"


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_c(tables, out):
    lines = ["/* Generated by tools/export-tables/gen_export_table.py from tables.json - do not edit. */",
             "/* Offsets are per firmware build of each library, checked by the anchor string. */",
             "#ifndef HMD2_EXPORTS_H",
             "#define HMD2_EXPORTS_H",
             ""]
    for lib, entry in tables["libraries"].items():
        for fw in sorted(entry["firmware"], key=int, reverse=True):
            table = entry["firmware"][fw]
            lines.append(f"/* {lib} {fw_name(fw)} ({table.get('covers', '')}) */")
            lines.append(f"static const export_sig_t kExports_{lib}_{fw}[] = {{")
            for name, offset in table["exports"].items():
                lines.append(f'    {{"{name}", 0x{offset:x}u}},')
            lines.append("};")
            anchor = table["anchor"]
            lines.append(f"static const export_anchor_t kAnchor_{lib}_{fw} = "
                         f'{{0x{anchor["va"]:x}u, {c_string(anchor["text"])}}};')
            lines.append("")
        lines.append(f"static const export_table_t kTables_{lib}[] = {{")
        for fw in sorted(entry["firmware"], key=int, reverse=True):
            lines.append(f"    {{{fw}, kExports_{lib}_{fw}, "
                         f"sizeof kExports_{lib}_{fw} / sizeof *kExports_{lib}_{fw}, "
                         f"&kAnchor_{lib}_{fw}}},")
        lines.append("};")
        lines.append("")
    lines.append("#endif /* HMD2_EXPORTS_H */")
    text = "\n".join(lines) + "\n"
    if out is None:
        sys.stdout.write(text)
    else:
        out.write_text(text)
        print(f"wrote {out} ({len(text.splitlines())} lines)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit-c", action="store_true", help="write the C header from tables.json")
    parser.add_argument("-o", "--output", type=Path, help="output file for --emit-c (default: stdout)")
    parser.add_argument("--tables", type=Path, default=TABLES, help="table data (default: tables.json next to this script)")
    parser.add_argument("--sprx", type=Path, help="a decrypted .sprx to read")
    parser.add_argument("--lib", help="library name, e.g. libSceHmd2")
    parser.add_argument("--fw", help="firmware tag, e.g. 1360 (default: the newest in the tables for --lib)")
    parser.add_argument("--names", help="comma-separated exports (default: those in the tables for --lib/--fw)")
    parser.add_argument("--verify", action="store_true", help="compare --sprx against the tables, exit 1 on a difference")
    args = parser.parse_args()

    tables = json.loads(args.tables.read_text())

    if args.emit_c:
        emit_c(tables, args.output)
        return 0

    if not args.sprx or not args.lib:
        parser.error("--sprx and --lib are required (or use --emit-c)")

    known = tables["libraries"].get(args.lib, {}).get("firmware", {})
    fw = args.fw or max(known, key=int, default=None)
    if not fw:
        parser.error(f"no table for {args.lib} yet: pass --fw and --names")
    names = [n for n in (args.names or "").split(",") if n] or list(known.get(fw, {}).get("exports", {}))
    if not names:
        parser.error(f"no names to look up for {args.lib} {fw}: pass --names")
    if args.names and args.fw is None:
        parser.error("--names needs --fw (which table the names belong to)")

    sprx = Sprx(args.sprx)
    exports = sprx.exports()
    va, text = sprx.anchor()
    by_nid = {}
    for want in names:
        by_nid.setdefault(nid_of(want), want)

    found, missing = {}, []
    for symbol, value in exports.items():
        name = symbol if symbol in names else by_nid.get(symbol)
        if name:
            found[name] = value
    missing = [n for n in names if n not in found]
    if missing:
        sys.exit(f"{args.sprx}: no export named {', '.join(missing)}")

    entry = {"anchor": {"va": va, "text": text}, "exports": dict(sorted(found.items()))}

    if not args.verify:
        json.dump(entry, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return 0

    have = known.get(fw)
    if have is None:
        print(f"{args.lib} {fw}: no table in {args.tables}", file=sys.stderr)
        return 1
    bad = 0
    for name, value in have["exports"].items():
        if found.get(name) != value:
            print(f"{args.lib} {fw}: {name} is 0x{value:x} in the tables, "
                  f"0x{found[name]:x} here" if name in found else
                  f"{args.lib} {fw}: {name} is missing here", file=sys.stderr)
            bad += 1
    for name in found:
        if name not in have["exports"]:
            print(f"{args.lib} {fw}: {name} 0x{found[name]:x} is not in the tables", file=sys.stderr)
            bad += 1
    if (va, text) != (have["anchor"]["va"], have["anchor"]["text"]):
        print(f"{args.lib} {fw}: anchor is 0x{va:x} {text!r} here, "
              f"0x{have['anchor']['va']:x} {have['anchor']['text']!r} in the tables", file=sys.stderr)
        bad += 1
    if bad:
        return 1
    print(f"{args.lib} {fw}: {len(found)} exports and the anchor match", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
