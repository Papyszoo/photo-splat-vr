#!/usr/bin/env bash
# Builds PS5 RADV (release) inside the amd64 container and copies the archive,
# its Vulkan headers and the payload SDK fork it was built against to /out.
#
# Inputs (copied by build-in-docker.sh with docker cp, or bind-mounted):
#   /PS5_Vulkan /PS5_Mesa /PS5_PayloadSDK   clones at SOURCE_ROOT (default /)
#   /work                                    writable scratch
#   /out                                     result directory
# Environment:
#   PS5_MESA_REVISION, PS5_PAYLOAD_SDK_REVISION (full commit ids)
#   SOURCE_ROOT (default /) where PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK live
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

: "${PS5_MESA_REVISION:?PS5_MESA_REVISION is unset}"
: "${PS5_PAYLOAD_SDK_REVISION:?PS5_PAYLOAD_SDK_REVISION is unset}"
jobs=${BUILD_JOBS:-2}

# The build writes into its own trees: copy the read-only clones once. With
# build-in-docker.sh the clones arrive by docker cp at /, so SOURCE_ROOT=/;
# a bind-mounted /ref works too.
source_root=${SOURCE_ROOT:-/ref}
src=/work/src
if [[ ! -f $src/.copied ]]; then
    rm -rf "$src"
    mkdir -p "$src"
    for name in PS5_Vulkan PS5_Mesa PS5_PayloadSDK; do
        cp -a "$source_root/$name" "$src/$name"
    done
    touch "$src/.copied"
fi

# docker cp keeps the host's numeric ownership; git refuses a tree owned by
# another uid unless it is listed.
for name in PS5_Vulkan PS5_Mesa PS5_PayloadSDK; do
    git config --global --add safe.directory "$src/$name"
done

export PS5_VULKAN_ROOT="$src/PS5_Vulkan"
export PS5_MESA_FORK="$src/PS5_Mesa"
export PS5_PAYLOAD_SDK_FORK="$src/PS5_PayloadSDK"
export PS5_MESA_REVISION
export PS5_PAYLOAD_SDK="$PS5_VULKAN_ROOT/.deps/native/ps5-payload-sdk"
export LLVM_CONFIG=${LLVM_CONFIG:-/usr/bin/llvm-config-19}

# Upstream's build-radv.sh calls Ninja without a job limit; two jobs fit the
# 5 GiB the container is given (PS5LM's CI does the same).
mkdir -p /work/bin
printf '#!/bin/sh\nexec /usr/bin/ninja -j "%s" "$@"\n' "$jobs" > /work/bin/ninja
chmod +x /work/bin/ninja
export NINJA=/work/bin/ninja

echo "==> [radv] Mesa $(git -C "$PS5_MESA_FORK" rev-parse --short=12 "$PS5_MESA_REVISION")"
echo "==> [radv] SDK fork $(git -C "$PS5_PAYLOAD_SDK_FORK" rev-parse --short=12 "$PS5_PAYLOAD_SDK_REVISION")"
echo "==> [radv] Vulkan $(git -C "$PS5_VULKAN_ROOT" rev-parse --short=12 HEAD)"

# PS5_Vulkan's bootstrapper pins an older SDK fork (b83202b) than the one
# RADV's own link recipe now expects: readlink and mkstemp are among the libc
# shims added later, and a payload (unlike a title with its own libc.prx) has
# no other provider. Install the requested revision first, then let the
# bootstrapper add only zlib (--skip-sdk).
sdk="$PS5_VULKAN_ROOT/.deps/native/ps5-payload-sdk"
if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$PS5_PAYLOAD_SDK_REVISION" ]]; then
    tree=/work/sdk-src
    rm -rf "$tree"
    mkdir -p "$tree" /work/cache
    git -C "$PS5_PAYLOAD_SDK_FORK" archive "$PS5_PAYLOAD_SDK_REVISION" | tar -x -C "$tree"
    bash "$tree/platform/tools/setup-sdk.sh" "$sdk" "$PS5_PAYLOAD_SDK_REVISION" /work/cache
    rm -rf "$tree"
fi

bash "$PS5_VULKAN_ROOT/tools/setup-native-dependencies.sh" --skip-sdk

start=$(date +%s)
cd "$PS5_VULKAN_ROOT"
bash tools/build-radv.sh release
seconds=$(( $(date +%s) - start ))

install="$PS5_VULKAN_ROOT/.deps/native/radv-release"
archive="$install/lib/libvulkan_radeon.ps5.a"
[[ -f $archive ]] || { echo "no $archive after the build" >&2; exit 1; }

builtins=/usr/lib/llvm-19/lib/clang/19/lib/linux/libclang_rt.builtins-x86_64.a
[[ -f $builtins ]] || { echo "no $builtins" >&2; exit 1; }

rm -rf /out/lib /out/include /out/sdk
mkdir -p /out/lib
cp "$archive" /out/lib/libvulkan_radeon.ps5.a
cp "$builtins" /out/lib/libclang_rt.builtins-x86_64.a
cp -a "$install/include" /out/include
# The SDK the archive was built and is linked against, with libps5platform.a.
cp -a "$PS5_PAYLOAD_SDK" /out/sdk

# AGC import stubs, as PS5_Vulkan's title build makes them: the v0.42 SDK has
# no libSceAgc/libSceAgcDriver stub, and RADV's PS5 winsys imports five AGC
# entry points. Each .so carries the system module's SONAME, so a payload's
# loader resolves the imports from the console's own libSceAgc.sprx. The
# canary sources are host-link declarations and are never executed.
for pair in libSceAgc:agc_canary_link_stub.c libSceAgcDriver:agc_driver_canary_link_stub.c; do
    library=${pair%%:*}
    source=${pair#*:}
    "$PS5_PAYLOAD_SDK/bin/prospero-clang" -std=c11 -O2 -fPIC \
        -c "$PS5_VULKAN_ROOT/vendor/ps5/sdk/stubs/$source" -o "/work/$library.o"
    "$PS5_PAYLOAD_SDK/bin/prospero-lld" --shared -soname "$library.sprx" \
        -o "/out/lib/$library.so" "/work/$library.o"
done

{
    echo "PS5 RADV release archive for ps5-splat-vr Spike B"
    echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "container: $(. /etc/os-release && echo "$PRETTY_NAME"), $(uname -m)"
    echo "ps5_vulkan: $(git -C "$PS5_VULKAN_ROOT" rev-parse HEAD)"
    echo "ps5_mesa: $(git -C "$PS5_MESA_FORK" rev-parse HEAD)"
    echo "ps5_payload_sdk: $PS5_PAYLOAD_SDK_REVISION"
    echo "ps5_mesa_patches: none (PS5LM's patches/mesa are app-specific and not applied)"
    echo "llvm: $(/usr/bin/llvm-config-19 --version)"
    echo "build_seconds: $seconds"
    echo "build_jobs: $jobs"
    echo "archive_sha256: $(sha256sum /out/lib/libvulkan_radeon.ps5.a | cut -d' ' -f1)"
    echo "archive_bytes: $(stat -c %s /out/lib/libvulkan_radeon.ps5.a)"
    echo "builtins_sha256: $(sha256sum /out/lib/libclang_rt.builtins-x86_64.a | cut -d' ' -f1)"
    echo "agc_stub_sha256: $(sha256sum /out/lib/libSceAgc.so /out/lib/libSceAgcDriver.so | cut -d' ' -f1 | tr '\n' ' ')"
} > /out/BUILD.txt

cat /out/BUILD.txt
echo "==> [radv] done in ${seconds}s"
