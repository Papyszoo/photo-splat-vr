#!/usr/bin/env bash
# Static checks on the vr_hello title package (no console, nothing sent).
#
#   platform/ps5/vr_hello/verify-package.sh build/ps5/title/PPSA99202
#
# The same checks viewer/verify-package.sh makes, for this package:
#
#   - eboot.bin is a signed, plaintext fSELF with valid integrity;
#   - param.json names the title the folder is named after, and carries the PS
#     VR2 identity HLA-PSVR2 ships (attribute 0x62000000, attribute3 0x4080c40:
#     PS VR2 app | VR-only | 120 Hz);
#   - sce_module/libc.prx is the published companion module (SHA-256);
#   - the linked PIE (build/ps5/title/obj/<titleId>/vr_hello_title/eboot.elf, when
#     it is still there) needs VideoOut / SystemService / kernel, exports nothing,
#     has its entry point, keeps the Vulkan shims local, and carries the two
#     embedded SPIR-V pattern shaders (the title reads no shader files at run time).
#
# Exit: 0 all checks pass, 1 a check failed, 2 usage/no package.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
: "${SPLAT_DATA_DIR:=$(cd -- "$repo/.." && pwd)}"

package="${1:-}"
[ -n "$package" ] || { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ -d "$package" ] || { echo "verify-package: no such directory: $package" >&2; exit 2; }
[ -f "$package/eboot.bin" ] || { echo "verify-package: $package has no eboot.bin" >&2; exit 2; }

tool="${PS5_NATIVE_TOOL:-$SPLAT_DATA_DIR/ref/PS5VR/app/tools/native-mac/ps5-native-tool}"
[ -x "$tool" ] || { echo "verify-package: no ps5-native-tool at $tool (set PS5_NATIVE_TOOL)" >&2; exit 2; }

readelf=""
for candidate in "$(dirname "${LLVM_CONFIG:-/nonexistent}")/llvm-readelf" \
                 /opt/homebrew/opt/llvm@21/bin/llvm-readelf \
                 "$(command -v llvm-readelf || true)" \
                 "$(command -v readelf || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then readelf="$candidate"; break; fi
done

failed=0
pass() { printf '  OK   %s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; failed=1; }

echo "verify-package: $package"

# ---- the signed container ---------------------------------------------------
inspect="$("$tool" self --inspect --file "$package/eboot.bin" 2>&1)" || {
    fail "eboot.bin does not inspect as a SELF"
    inspect=""
}
if printf '%s\n' "$inspect" | grep -q 'container: signed, plaintext'; then
    pass "eboot.bin: signed, plaintext ($(wc -c <"$package/eboot.bin" | tr -d ' ') bytes)"
else
    fail "eboot.bin is not a signed plaintext fSELF"
fi
if printf '%s\n' "$inspect" | grep -q 'integrity: valid'; then
    pass "eboot.bin: integrity valid"
else
    fail "eboot.bin integrity is not valid"
fi

# ---- param.json -------------------------------------------------------------
title_id="$(basename -- "$package")"
param="$package/sce_sys/param.json"
param_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$param" 2>/dev/null || true)"
if [ "$param_id" = "$title_id" ]; then
    pass "sce_sys/param.json: titleId $param_id"
else
    fail "sce_sys/param.json says '${param_id:-<unreadable>}', the folder is $title_id"
fi
vr="$(python3 - "$param" 2>/dev/null <<'PY' || true
import json, sys
doc = json.load(open(sys.argv[1]))
print("0x%08x 0x%08x" % (int(doc.get("attribute", 0)), int(doc.get("attribute3", 0))))
PY
)"
if [ "$vr" = "0x62000000 0x04080c40" ]; then
    pass "sce_sys/param.json: VR identity attribute 0x62000000 attribute3 0x04080c40"
else
    fail "sce_sys/param.json VR identity is '${vr:-<unreadable>}', need 0x62000000 0x04080c40"
fi

# ---- the companion module ---------------------------------------------------
expected_libc=e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
actual_libc="$(shasum -a 256 "$package/sce_module/libc.prx" 2>/dev/null | awk '{print $1}')"
if [ "$actual_libc" = "$expected_libc" ]; then
    pass "sce_module/libc.prx: SHA-256 matches the published module"
else
    fail "sce_module/libc.prx SHA-256 ${actual_libc:-<missing>} != $expected_libc"
fi

# ---- the linked PIE ---------------------------------------------------------
# Intermediates are per identity (build-title.mk): build/ps5/title/obj/<ID>/
# vr_hello_title/. The older location next to the package is still accepted so a
# package from a previous worktree can be inspected.
pie=""
for candidate in "$(dirname -- "$package")/obj/$title_id/vr_hello_title" \
                 "$(dirname -- "$package")/vr_hello_title"; do
    if [ -f "$candidate/eboot.elf" ]; then
        pie="$candidate/eboot.elf"
        break
    fi
done
if [ -f "$pie" ] && [ -n "$readelf" ]; then
    needed="$("$readelf" -d "$pie" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')"
    for module in libSceVideoOut.prx libSceSystemService.prx libkernel.prx; do
        if printf '%s\n' "$needed" | grep -qx "$module"; then
            pass "eboot.elf: needs $module"
        else
            fail "eboot.elf does not name $module (VideoOut flips the eye buffers)"
        fi
    done
    exports="$("$readelf" --dyn-syms "$pie" | awk '$4=="FUNC" && $7!="UND"' | wc -l | tr -d ' ')"
    if [ "$exports" = "0" ]; then
        pass "eboot.elf: no exported function (the converter refuses exports)"
    else
        fail "eboot.elf exports $exports function(s)"
    fi
    entry="$("$readelf" -h "$pie" | sed -n 's/.*Entry point address: *//p')"
    if [ -n "$entry" ] && [ "$entry" != "0x0" ]; then
        pass "eboot.elf: entry $entry (_start)"
    else
        fail "eboot.elf has no entry point"
    fi
    local_shims="$("$readelf" --dyn-syms "$pie" | awk '$8 ~ /^vk[A-Z]/ && $7!="UND"' | wc -l | tr -d ' ')"
    if [ "$local_shims" = "0" ]; then
        pass "eboot.elf: the Vulkan shims are local"
    else
        fail "eboot.elf exports $local_shims Vulkan shim(s)"
    fi
    embedded="$(python3 - "$pie" "$(dirname -- "$package")/gen" 2>/dev/null <<'PY' || true
import os, re, sys
image = open(sys.argv[1], "rb").read()
found = 0
for name in ("pattern_vert.h", "pattern_frag.h"):
    header = os.path.join(sys.argv[2], name)
    if not os.path.exists(header):
        continue
    text = open(header).read()
    words = [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]{8})", text)][:16]
    if not words:
        continue
    prefix = b"".join(word.to_bytes(4, "little") for word in words)
    if prefix in image:
        found += 1
print(found)
PY
)"
    if [ "${embedded:-0}" = "2" ]; then
        pass "eboot.elf: both embedded SPIR-V pattern shaders present"
    else
        fail "eboot.elf carries ${embedded:-0} of the 2 embedded pattern shaders (build with make vr-hello-title)"
    fi
else
    echo "  --   eboot.elf not built (obj/$title_id/vr_hello_title or vr_hello_title next to the package); skipping the ELF checks"
fi

if [ "$failed" -ne 0 ]; then
    echo "verify-package: FAIL"
    exit 1
fi
echo "verify-package: PASS"
