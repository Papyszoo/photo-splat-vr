#!/usr/bin/env bash
# Static checks for the three Spike A payloads, without a console.
#
#   platform/ps5/bench/verify-elfs.sh            # the three ELFs + the title PIE
#   platform/ps5/bench/verify-elfs.sh foo.elf    # any payload
#
# When the bench title has been built (make bench-title), its linked PIE is
# checked as well: that is the ELF carrying BENCH=http, and its imports are the
# ones the console must provide. Only its import + PIE checks apply (see the
# comment before that block).
#
# The check itself is platform/ps5/vk_hello/verify-elf.sh, which Spike B built
# and used for vk_hello/probe_agc: DYN + FLAGS_1 PIE + no TEXTREL, entry 0,
# no PT_INTERP, 0x4000 LOAD alignment, and every undefined dynamic symbol
# resolved by one of the NEEDED SDK stubs. Reused rather than copied so the two
# spikes cannot drift.
#
# Results are also written to ${SPLAT_DATA_DIR:-<repo parent>}/runs/A5/.
# Exit: 0 all passed, 1 a payload failed, 2 usage/missing file.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"
check="$here/../vk_hello/verify-elf.sh"
data="${SPLAT_DATA_DIR:-$(cd -- "$repo/.." && pwd)}"
runs="$data/runs/A5"

[ -x "$check" ] || { echo "verify-elfs: missing $check (Spike B's static checker)" >&2; exit 2; }

if [ "$#" -gt 0 ]; then
    elf_list=("$@")
else
    elf_list=("$here/op_audit.elf" "$here/da2_bench.elf" "$here/sharp_bench.elf")
fi

mkdir -p "$runs"
failed=0
for elf in "${elf_list[@]}"; do
    name="$(basename -- "$elf")"
    [ -f "$elf" ] || { echo "verify-elfs: no $elf (make -C platform/ps5/bench)" >&2; exit 2; }
    log="$runs/verify-$name.txt"
    echo "== $name =="
    if "$check" "$elf" >"$log" 2>&1; then
        grep -E '^(size|NEEDED|undefined|weak,|symbol check|PIE check|entry|PT_INTERP|LOAD|static verification)' "$log"
        echo
    else
        echo "FAIL (see $log)"
        tail -20 "$log"
        failed=1
    fi
done

# The bench title is the ELF that carries BENCH=http. Check its imports too,
# on the pre-conversion PIE: the .so stubs export symbol *names*, while
# ps5-native-tool link turns the signed eboot's imports into NIDs, and a title
# has a real entry point, so verify-elf.sh's payload-layout section (entry 0,
# elfldr layout) does not apply to it. The import and PIE checks do.
title_pie="${TITLE_PIE:-$repo/build/ps5/title/bench_title/llvm-pie.elf}"
if [ -f "$title_pie" ]; then
    log="$runs/verify-title-bench_title.txt"
    echo "== bench_title (title PIE, BENCH=http) =="
    "$check" "$title_pie" >"$log" 2>&1 || true
    if grep -q '^symbol check: PASS' "$log" && grep -q '^PIE check: PASS' "$log"; then
        grep -E '^(size|NEEDED|undefined|symbol check|PIE check)' "$log"
        echo "title PIE: import + PIE checks PASS (payload layout does not apply)"
        echo
    else
        echo "FAIL (see $log)"
        tail -20 "$log"
        failed=1
    fi
else
    echo "== bench_title (title PIE, BENCH=http) =="
    echo "not built (make -C platform/ps5/bench bench-title); title import check skipped"
    echo
fi

if [ "$failed" -eq 0 ]; then
    echo "all payloads: static verification PASS"
else
    echo "static verification FAIL" >&2
fi
exit "$failed"
