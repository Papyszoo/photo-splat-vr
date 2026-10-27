#!/usr/bin/env bash
# One command on the console: upload vk_hello.elf through ProsperoMgr, launch
# it and capture klog. Wraps platform/ps5/scripts/run.sh with the settings the
# 97 MiB first upload and the ELF's start line need.
#
#   platform/ps5/vk_hello/run-on-console.sh        # 120 s capture
#   platform/ps5/vk_hello/run-on-console.sh 30     # shorter capture
#
# PS5_HOST (and the other console settings) come from the environment or the
# repo's gitignored .env.local. Build the ELF first; see docs/research/spike-b.md.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
elf=$here/vk_hello.elf
seconds=${1:-120}

[[ -f $elf ]] || {
    echo "no $elf" >&2
    echo "build it: PS5_PAYLOAD_SDK=\"\$SPLAT_DATA_DIR/ref/radv-ps5/sdk\" make -C $here" >&2
    exit 2
}

# The start line names the payload ("vk_hello: start"); run.sh's default
# pattern is the ELF stem, which matches. The HTTP timeout covers the first
# 97 MiB upload, which can exceed the 30 s default on Wi-Fi.
exec env PS5_HTTP_TIMEOUT="${PS5_HTTP_TIMEOUT:-180}" \
    "$here/../scripts/run.sh" "$elf" "$seconds"
