#!/usr/bin/env bash
# Builds the PS5 RADV release archive in an amd64 Debian container and installs
# it where platform/ps5/vk_hello expects it:
#
#   $SPLAT_DATA_DIR/ref/radv-ps5/
#     lib/libvulkan_radeon.ps5.a          RADV + ACO + NIR + Mesa's runtime
#     lib/libclang_rt.builtins-x86_64.a   Clang builtins (__emutls_get_address)
#     lib/libSceAgc.so, lib/libSceAgcDriver.so   AGC import stubs
#     include/vulkan, include/vk_video    the headers it was built with
#     sdk/                                PS5_PayloadSDK pin + libps5platform.a
#     BUILD.txt                           revisions, system, time, hashes
#
# The sources come from the clones under $SPLAT_DATA_DIR/ref (PS5_Vulkan,
# PS5_Mesa, PS5_PayloadSDK), copied into the container with docker cp. Docker
# needs ~5 GiB of memory and a linux/amd64 runtime; the container is capped at
# 2 CPUs and 5 GiB.
#
#   platform/ps5/radv/build-in-docker.sh                        # build
#   RADV_REBUILD_IMAGE=1 platform/ps5/radv/build-in-docker.sh   # new image
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
data=${SPLAT_DATA_DIR:-$(cd -- "$root/.." && pwd)}
out=${RADV_PS5:-$data/ref/radv-ps5}
work=${RADV_WORK:-$data/runs/B1/radv-docker}

# Revisions: PS5_Vulkan at f410e67 pins Mesa 1ba9874. Its own dependency
# bootstrapper pins SDK fork b83202b, but RADV's link recipe needs the libc
# shims (ps5_readlink, ps5_mkstemp, ps5_localeconv, ...) added afterwards, and
# a payload has no title libc.prx to supply them: use the fork's current
# revision 3e31e0b. The archive is rebuilt against it, so one SDK is in play.
mesa=1ba9874f478657ccf6da72ce55f0260164cc0bd0
sdk=3e31e0b1b68ada28beca68adc480cf0c3d17b9df
image=ps5-radv-build:trixie-llvm19

for path in "$data/ref/PS5_Vulkan" "$data/ref/PS5_Mesa" "$data/ref/PS5_PayloadSDK"; do
    [[ -d $path/.git ]] || { echo "missing clone: $path" >&2; exit 2; }
done
git -C "$data/ref/PS5_Vulkan" cat-file -e f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed^{commit} ||
    { echo "PS5_Vulkan is not at the pinned history" >&2; exit 2; }
git -C "$data/ref/PS5_Mesa" cat-file -e "$mesa^{commit}" ||
    { echo "PS5_Mesa has no $mesa" >&2; exit 2; }
git -C "$data/ref/PS5_PayloadSDK" cat-file -e "$sdk^{commit}" ||
    { echo "PS5_PayloadSDK has no $sdk" >&2; exit 2; }

mkdir -p "$out" "$work"
log=$work/build.log

if [[ ${RADV_REBUILD_IMAGE:-0} == 1 ]] || ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build --platform linux/amd64 -t "$image" \
        -f "$root/platform/ps5/radv/Dockerfile" "$root/platform/ps5/radv"
fi

# No bind mount from the repo/data dir: colima shares /Users and /tmp but not
# the external /Volumes, and a bind source it does not share silently becomes
# an empty directory in the VM. docker cp moves the clones and the result.
name=${RADV_CONTAINER:-ps5-radv-build}
docker rm -f "$name" >/dev/null 2>&1 || true
docker create --name "$name" --platform linux/amd64 \
    --cpus 2 --memory 5g --memory-swap 5g \
    -e PS5_MESA_REVISION="$mesa" \
    -e PS5_PAYLOAD_SDK_REVISION="$sdk" \
    -e BUILD_JOBS=2 \
    -e SOURCE_ROOT=/ \
    "$image" >/dev/null

for clone in PS5_Vulkan PS5_Mesa PS5_PayloadSDK; do
    echo "copying $clone into the container"
    docker cp "$data/ref/$clone" "$name:/"
done

set +e
docker start -a "$name" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set -e
if ((rc != 0)); then
    echo "build failed (exit $rc); container '$name' kept for inspection" >&2
    exit "$rc"
fi

docker cp "$name:/out/." "$out/"
docker rm -f "$name" >/dev/null

echo "RADV archive installed in $out"
