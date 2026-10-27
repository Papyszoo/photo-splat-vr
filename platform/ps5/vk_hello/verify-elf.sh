#!/usr/bin/env bash
# Static check of vk_hello.elf (or any payload) without a console:
#   - ELF is an x86-64 PS5 PIE payload (DYN, FLAGS_1 PIE, entry 0)
#   - every undefined dynamic symbol is exported by one of the NEEDED SDK stubs
#   - no TEXTREL, no unexpected NEEDED entries
#   - size and section sizes
#
#   platform/ps5/vk_hello/verify-elf.sh [elf]
#
# The SDK defaults to $SPLAT_DATA_DIR/ref/radv-ps5/sdk (the tree the archive
# was linked against); READELF/NM come from Homebrew LLVM when not on PATH.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

elf=${1:-vk_hello.elf}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
data=${SPLAT_DATA_DIR:-$(cd -- "$here/../../../.." && pwd)}
sdk=${RADV_SDK:-$data/ref/radv-ps5/sdk}

readelf=${LLVM_READELF:-$(command -v llvm-readelf || true)}
nm=${LLVM_NM:-$(command -v llvm-nm || true)}
[[ -n $readelf ]] || readelf=/opt/homebrew/opt/llvm@21/bin/llvm-readelf
[[ -n $nm ]] || nm=/opt/homebrew/opt/llvm@21/bin/llvm-nm
[[ -x $readelf && -x $nm ]] || { echo "llvm-readelf/llvm-nm not found" >&2; exit 2; }
[[ -f $elf ]] || { echo "no $elf (build it first)" >&2; exit 2; }
[[ -d $sdk/target/lib ]] || { echo "no SDK stubs under $sdk/target/lib" >&2; exit 2; }

echo "== file =="
"$readelf" -h "$elf" | grep -E 'Class:|Type:|Machine:|Entry point'
size=$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$elf")
printf 'size: %s bytes (%.1f MiB)\n' "$size" "$(python3 -c "print($size/1048576)")"

echo
echo "== dynamic =="
"$readelf" -d "$elf" | grep -E 'NEEDED|FLAGS|FLAGS_1|TEXTREL' || true

echo
echo "== sections =="
"$readelf" -S --wide "$elf" | awk '$2 ~ /^\.(text|rodata|data|bss|dynsym|eh_frame)/ {print $2, $6, $7}'

python3 - "$elf" "$sdk" "$readelf" "$nm" <<'PY'
import re, subprocess, sys, pathlib

elf, sdk, readelf, nm = sys.argv[1:5]
out = subprocess.run([readelf, "--dyn-syms", "--wide", elf], text=True, capture_output=True, check=True).stdout
dyn = subprocess.run([readelf, "-d", elf], text=True, capture_output=True, check=True).stdout

needed = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", dyn)
undef, weak = set(), set()
for line in out.splitlines():
    parts = line.split()
    # Num: Value Size Type Bind Vis Ndx Name
    if len(parts) < 8 or not parts[0].endswith(":"):
        continue
    typ, bind, ndx, name = parts[3], parts[4], parts[6], parts[7]
    if typ != "FUNC" and typ != "OBJECT" and typ != "NOTYPE":
        continue
    if ndx != "UND":
        continue
    (weak if bind == "WEAK" else undef).add(name)

def exports(lib):
    for directory in (pathlib.Path(sdk, "target/lib"), pathlib.Path(sdk).parent / "lib"):
        path = directory / lib.replace(".sprx", ".so")
        if path.exists():
            break
    if not path.exists():
        return set(), path
    text = subprocess.run([nm, "--defined-only", "-g", "--format=posix", str(path)],
                          text=True, capture_output=True, check=True).stdout
    names = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in "TWtw":
            names.add(fields[0])
    return names, path

provided, missing_libs = set(), []
for lib in needed:
    names, path = exports(lib)
    if not path.exists():
        missing_libs.append(str(path))
    provided |= names

unresolved = sorted(undef - provided)
weak_unresolved = sorted(weak - provided)

print()
print("== imports ==")
print(f"NEEDED: {', '.join(needed) if needed else '(none)'}")
print(f"undefined: {len(undef)} strong, {len(weak)} weak")
print(f"exported by those stubs: {len(provided)} names")
for name in sorted(undef):
    print(f"  {name}")

ok = True
if missing_libs:
    ok = False
    print("MISSING STUB LIBRARIES:")
    for path in missing_libs:
        print(f"  {path}")
if unresolved:
    ok = False
    print(f"UNRESOLVED strong imports ({len(unresolved)}):")
    for name in unresolved:
        print(f"  {name}")
if weak_unresolved:
    print(f"weak, may resolve to 0 ({len(weak_unresolved)}): {', '.join(weak_unresolved)}")
print("symbol check:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY

echo
echo "== PIE =="
header=$("$readelf" -h "$elf" 2>/dev/null)
dynamic=$("$readelf" -d "$elf" 2>/dev/null)
programs=$("$readelf" -l "$elf" 2>/dev/null)
if grep -E 'Type:.*DYN' <<<"$header" >/dev/null &&
   grep -E 'FLAGS_1.*PIE' <<<"$dynamic" >/dev/null &&
   ! grep -E 'TEXTREL' <<<"$dynamic" >/dev/null; then
    echo "PIE check: PASS (DYN, FLAGS_1 PIE, no TEXTREL)"
else
    echo "PIE check: FAIL"
    exit 1
fi

echo
echo "== payload layout =="
entry=$(grep -E 'Entry point address:' <<<"$header" | awk '{print $NF}')
if [[ $entry == 0x0 ]]; then
    echo "entry: PASS (0x0; elfldr chooses the entry)"
else
    echo "entry: FAIL ($entry, expected 0x0)"
    exit 1
fi
if grep -E 'INTERP' <<<"$programs" >/dev/null; then
    echo "PT_INTERP: FAIL (a payload has no interpreter)"
    exit 1
fi
echo "PT_INTERP: PASS (none)"
if grep -E '^  LOAD' <<<"$programs" | grep -vE '0x4000$' >/dev/null; then
    echo "LOAD alignment: FAIL (a LOAD segment is not aligned to 0x4000)"
    exit 1
fi
echo "LOAD alignment: PASS (all LOAD segments 0x4000)"

echo
echo "static verification: PASS"
