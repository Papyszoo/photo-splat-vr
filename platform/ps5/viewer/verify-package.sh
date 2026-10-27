#!/usr/bin/env bash
# Static checks on the viewer title package (no console, nothing sent).
#
#   platform/ps5/viewer/verify-package.sh build/ps5/title/PPSA99202
#
# Checks what the console will load, the way verify-elf.sh does for the payload
# ELFs (platform/ps5/vk_hello/verify-elf.sh):
#
#   - eboot.bin is a signed, plaintext fSELF with valid integrity and carries
#     its identity's data folder (SPLAT_DATA_ROOT: /data/ps5-splat-vr for the
#     dev build, the release one's own folder for the release build);
#   - param.json names the title the folder is named after, its contentId is
#     exactly 36 characters carrying that id (IV0000-<id>_00-<16 chars>), its
#     titleName is the one platform/ps5/title/identities.json gives that id
#     (the uninstall proof and the tile), and it carries the PS VR2 identity
#     that made vr_hello work on the console
#     (attribute 0x62000000, attribute3 0x4080c40: PS VR2 app | VR-only | 120 Hz
#     | hand tracking);
#   - sce_module/libc.prx is the published companion module (SHA-256);
#   - the nine SPIR-V modules the C6 renderer loads (including the two overlay
#     shaders the UI panel needs) are present and start with the SPIR-V magic
#     (the title reads them from /app0/shaders);
#   - helper/splat-helper.elf, the capture payload the title pushes to the
#     loader on 127.0.0.1:9021 itself, is present, an ELF and under 1 MiB
#     (the mailbox is not built for megabytes);
#   - samples/, when this build had $SPLAT_DATA_DIR/assets/samples/out, holds
#     scene folders (each with scene.psplat), the photo CREDITS.md, and no
#     model weights; a package built without samples simply has no such folder;
#   - the package carries nothing outside eboot.bin, sce_module, sce_sys,
#     shaders, helper and samples (the allow-list above);
#   - the linked PIE (build/ps5/title/viewer/eboot.elf, when it is still there)
#     needs the AGC, VideoOut, UserService, Pad and Sysmodule modules (the VR
#     path's Hmd2 session is for a user and takes the DualSense), exports
#     nothing (the title converter refuses exports) and has its Vulkan shims
#     local; the PIE source (llvm-pie.elf) still reserves at least half a page
#     of RELRO headroom for the process parameters ps5-native-tool writes;
#   - the VR path is in the image: the mode switch, the two log lines that
#     only a VR session prints and the TV mirror's line are literal strings in
#     the eboot.
#
# Exit: 0 all checks pass, 1 a check failed, 2 usage/no package.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
: "${SPLAT_DATA_DIR:=$(cd -- "$repo/.." && pwd)}"

package="${1:-}"
[ -n "$package" ] || { sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2; }
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

# The data folder is compiled in from the identity (build-title.mk:
# -DSPLAT_DATA_ROOT), not written in a source. A package whose eboot carries
# another identity's folder would write the other install's data.
want_data="$(python3 "$repo/platform/ps5/title/identity.py" --field dataDir \
    --title-id "$(basename -- "$package")" 2>/dev/null || true)"
if [ -n "$want_data" ] && grep -qaF -- "$want_data" "$package/eboot.bin"; then
    pass "eboot.bin: carries its data folder $want_data (SPLAT_DATA_ROOT)"
else
    fail "eboot.bin does not carry the identity's data folder '${want_data:-<unknown id>}' (SPLAT_DATA_ROOT from build-title.mk)"
fi

# ---- param.json -------------------------------------------------------------
title_id="$(basename -- "$package")"
identity_py="$repo/platform/ps5/title/identity.py"
param_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$package/sce_sys/param.json" 2>/dev/null || true)"
if [ "$param_id" = "$title_id" ]; then
    pass "sce_sys/param.json: titleId $param_id"
else
    fail "sce_sys/param.json says '${param_id:-<unreadable>}', the folder is $title_id"
fi

# The console shows this name, and title-uninstall.sh accepts this id only when
# the installed param.json carries it (identities.json is the one source).
want_name="$(python3 "$identity_py" --field titleName --title-id "$title_id" 2>/dev/null || true)"
param_name="$(python3 -c '
import json, sys
doc = json.load(open(sys.argv[1]))
print("\n".join(sorted({value["titleName"]
                        for value in doc.get("localizedParameters", {}).values()
                        if isinstance(value, dict) and value.get("titleName")})))' \
    "$package/sce_sys/param.json" 2>/dev/null || true)"
if [ -n "$want_name" ] && [ "$param_name" = "$want_name" ]; then
    pass "sce_sys/param.json: titleName \"$want_name\" (identities.json)"
else
    fail "sce_sys/param.json titleName is '${param_name:-<none>}', identities.json says '${want_name:-<unknown id>}' for $title_id"
fi

# The identity's contentId is exactly 36 characters and carries the title id:
# IV0000 + '-' + <titleId> + '_00' + '-' + 16 characters. The id this project
# shipped before the release split was 35 (a 15-character suffix), which a
# strict linter rejects (runs/release/PACKAGE-ID-verify.md, 4b). The publisher
# prefix must be IV0000, the homebrew one.
identity="$(python3 - "$package/sce_sys/param.json" "$title_id" <<'PYID' 2>/dev/null || true
import json, re, sys
path, title_id = sys.argv[1], sys.argv[2]
doc = json.load(open(path))
content_id = str(doc.get("contentId", ""))
concept_id = str(doc.get("conceptId", ""))
problems = []
if len(content_id) != 36:
    problems.append("contentId %r is %d characters, not 36" % (content_id, len(content_id)))
if not re.fullmatch(r"IV[0-9]{4}-%s_00-[A-Z0-9]{16}" % re.escape(title_id), content_id):
    problems.append("contentId %r is not IV0000-%s_00-<16 chars>" % (content_id, title_id))
if concept_id != title_id[4:]:
    problems.append("conceptId %r is not the five digits of %s" % (concept_id, title_id))
print("OK" if not problems else "FAIL " + "; ".join(problems))
PYID
)"
case "$identity" in
    OK*) pass "sce_sys/param.json: contentId 36 chars, conceptId ${param_id#PPSA}, IV0000 prefix" ;;
    *) fail "sce_sys/param.json identity: ${identity#FAIL }" ;;
esac
vr="$(python3 - "$package/sce_sys/param.json" 2>/dev/null <<'PYVR' || true
import json, sys
doc = json.load(open(sys.argv[1]))
print("0x%08x 0x%08x" % (int(doc.get("attribute", 0)), int(doc.get("attribute3", 0))))
PYVR
)"
if [ "$vr" = "0x62000000 0x04080c40" ]; then
    pass "sce_sys/param.json: VR identity attribute 0x62000000 attribute3 0x04080c40"
else
    fail "sce_sys/param.json VR identity is '${vr:-<unreadable>}', need 0x62000000 0x04080c40 (without it Hmd2 never reports worn and never consumes a frame)"
fi

# ---- the companion module ---------------------------------------------------
expected_libc=e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036
actual_libc="$(shasum -a 256 "$package/sce_module/libc.prx" 2>/dev/null | awk '{print $1}')"
if [ "$actual_libc" = "$expected_libc" ]; then
    pass "sce_module/libc.prx: SHA-256 matches the published module"
else
    fail "sce_module/libc.prx SHA-256 ${actual_libc:-<missing>} != $expected_libc"
fi

# ---- shaders ----------------------------------------------------------------
shaders="project.comp finalize.comp splat.vert splat.frag comfort.vert comfort.frag \
         overlay.vert overlay.frag overlay_rgba.frag"
for name in $shaders; do
    file="$package/shaders/$name.spv"
    if [ ! -f "$file" ]; then
        fail "shaders/$name.spv is missing (the renderer loads it from /app0/shaders)"
        continue
    fi
    magic="$(od -An -tx1 -N4 "$file" | tr -d ' \n')"
    if [ "$magic" = "03022307" ]; then
        pass "shaders/$name.spv: $(wc -c <"$file" | tr -d ' ') bytes, SPIR-V magic"
    else
        fail "shaders/$name.spv does not start with the SPIR-V magic (got $magic)"
    fi
done

# ---- the capture helper payload ---------------------------------------------
# A payload, not a title module: the viewer copies platform/ps5/helper/
# splat-helper.elf here and pushes it to elfldr on 127.0.0.1:9021 at run time
# (core/capture/helper_client.cpp). Keep it small: the whole file goes over
# that loopback socket.
helper="$package/helper/splat-helper.elf"
if [ -f "$helper" ]; then
    helper_size="$(wc -c <"$helper" | tr -d ' ')"
    helper_magic="$(od -An -tx1 -N4 "$helper" | tr -d ' \n')"
    if [ "$helper_magic" != "7f454c46" ]; then
        fail "helper/splat-helper.elf does not start with the ELF magic (got $helper_magic)"
    elif [ "$helper_size" -lt 1048576 ]; then
        pass "helper/splat-helper.elf: $helper_size bytes (< 1 MiB), ELF magic"
    else
        fail "helper/splat-helper.elf is $helper_size bytes, not under 1 MiB"
    fi
else
    fail "helper/splat-helper.elf is missing (the title starts it from /app0/helper)"
fi

# ---- the bundled sample scenes (W31) ---------------------------------------
# Optional by design: only a build made with $SPLAT_DATA_DIR/assets/samples/out
# carries them. They are ordinary scene folders plus the photo credits, never
# model weights (AGENTS.md).
samples_dir="$package/samples"
if [ -d "$samples_dir" ]; then
    scene_count="$(find "$samples_dir" -mindepth 2 -maxdepth 2 -type f -name scene.psplat | wc -l | tr -d ' ')"
    if [ "$scene_count" -ge 1 ]; then
        pass "samples/: $scene_count scene(s), $(du -sk "$samples_dir" | awk '{print $1 * 1024}') bytes"
    else
        fail "samples/ holds no scene folder with scene.psplat"
    fi
    if [ -f "$samples_dir/CREDITS.md" ]; then
        pass "samples/CREDITS.md: photo credits present"
    else
        fail "samples/CREDITS.md is missing (the photo credits ship with the samples)"
    fi
    weights="$(find "$samples_dir" -type f \( -name '*.pt' -o -name '*.gguf' -o -name '*.safetensors' \) | head -n 1)"
    if [ -z "$weights" ]; then
        pass "samples/: no model weights"
    else
        fail "samples/ carries a model weight: $weights"
    fi
else
    echo "  --   samples/ not present (this build has no bundled scenes)"
fi

# ---- the package allow-list -------------------------------------------------
# The folders this recipe installs are a closed set; a stray top-level entry
# would be installed with the title and never read.
for entry in "$package"/*; do
    [ -e "$entry" ] || continue
    name="$(basename -- "$entry")"
    case "$name" in
        eboot.bin|sce_module|sce_sys|shaders|helper|samples) ;;
        *) fail "unexpected package entry '$name' (allow-list: eboot.bin sce_module sce_sys shaders helper samples)" ;;
    esac
done

# ---- the linked PIE ---------------------------------------------------------
# Intermediates are per identity (build-title.mk): build/ps5/title/obj/<ID>/
# viewer/. The older location next to the package is still accepted so a
# package from a previous worktree can be inspected.
pie=""
pie_source=""
for candidate in "$(dirname -- "$package")/obj/$title_id/viewer" \
                 "$(dirname -- "$package")/viewer"; do
    if [ -f "$candidate/eboot.elf" ]; then
        pie="$candidate/eboot.elf"
        pie_source="$candidate/llvm-pie.elf"
        break
    fi
done
if [ -f "$pie" ] && [ -n "$readelf" ]; then
    needed="$("$readelf" -d "$pie" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')"
    for module in libSceVideoOut.prx libSceAgc.prx libSceAgcDriver.prx libSceUserService.prx \
                  libScePad.prx libSceSysmodule.prx; do
        if printf '%s\n' "$needed" | grep -qx "$module"; then
            pass "eboot.elf: needs $module"
        else
            fail "eboot.elf does not name $module (the VR path opens Hmd2 for a user, takes the pad and loads the VR modules)"
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
    # ps5-native-tool link writes the process-parameter block into the padding
    # between the last RELRO/rodata section and the page-aligned .dynamic, and
    # refuses the image when it does not fit (0x320 B today). The source PIE
    # reserves a whole page there (platform/ps5/title/ps5-pie-title.ld), so
    # reproduce the converter's computation (sce_module_writer.cpp) and keep
    # the headroom from drifting back to "whatever the page had left".
    if [ -f "$pie_source" ] && [ -n "$readelf" ]; then
        headroom="$(python3 - "$readelf" "$pie_source" <<'PYHEADROOM'
import re, subprocess, sys
readelf, path = sys.argv[1], sys.argv[2]
out = subprocess.run([readelf, "-SW", path], capture_output=True, text=True, check=True).stdout
pat = re.compile(r"^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+"
                 r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([A-Za-z]*)\s")

def metadata(name):
    return name in (".dynstr", ".dynsym", ".dynamic", ".hash", ".gnu.hash") \
        or name.startswith(".rela.")

def relro(name):
    return name in (".got", ".got.plt") or name.startswith(".data.rel.ro") \
        or name.startswith(".preinit_array") or name.startswith(".init_array") \
        or name.startswith(".fini_array")

dynamic = relro_start = ro_start = None
relro_end = ro_end = 0
for line in out.splitlines():
    m = pat.match(line)
    if not m or "A" not in m.group(7):
        continue
    name, kind, flags = m.group(1), m.group(2), m.group(7)
    address, size = int(m.group(3), 16), int(m.group(5), 16)
    end = address + size
    if name == ".dynamic":
        dynamic = address
    elif "X" in flags or "T" in flags:
        continue
    elif relro(name):
        relro_start = address if relro_start is None else min(relro_start, address)
        relro_end = max(relro_end, end)
    elif "W" in flags or kind == "NOBITS":
        continue
    elif not metadata(name):
        ro_start = address if ro_start is None else min(ro_start, address)
        ro_end = max(ro_end, end)
if dynamic is None:
    print(-1)
else:
    at = max(relro_end, ro_end if ro_start is not None and ro_end > relro_start else 0)
    print(dynamic - ((at + 7) & ~7))
PYHEADROOM
)"
        if [ "$headroom" -ge 8192 ]; then
            pass "llvm-pie.elf: $headroom B (0x$(printf '%x' "$headroom")) RELRO headroom for the process parameters"
        else
            fail "llvm-pie.elf leaves only $headroom B of RELRO headroom for the process parameters (ps5-native-tool needs 0x320; ps5-pie-title.ld reserves a page)"
        fi
    fi
    local_shims="$("$readelf" --dyn-syms "$pie" | awk '$8 ~ /^vk[A-Z]/ && $7!="UND"' | wc -l | tr -d ' ')"
    if [ "$local_shims" = "0" ]; then
        pass "eboot.elf: the Vulkan shims are local"
    else
        fail "eboot.elf exports $local_shims Vulkan shim(s)"
    fi
    for text in "MODE=vr" "viewer: vr eye buffer %d" "viewer: vr waiting for the headset" \
                "viewer: vr mirror:"; do
        if grep -qaF -- "$text" "$pie"; then
            pass "eboot.elf: carries \"$text\" (the VR path is linked in)"
        else
            fail "eboot.elf does not carry \"$text\": VR mode is missing from this build"
        fi
    done
else
    echo "  --   eboot.elf not present next to the package; skipping the ELF checks"
fi

if [ "$failed" -ne 0 ]; then
    echo "verify-package: FAIL"
    exit 1
fi
echo "verify-package: PASS"
