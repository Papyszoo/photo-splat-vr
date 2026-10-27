#!/usr/bin/env bash
# Configure and build the PS5 payload libraries (ggml CPU+Vulkan and the
# project's core sources, including core/render and its SPIR-V shaders, which
# the viewer title draws with) for the title/payload SDK target.
#
#   platform/ps5/ggml/build.sh                 # build-ps5-ggml/
#   platform/ps5/ggml/build.sh --clean         # wipe the build dir first
#   BUILD_DIR=build-ps5-other platform/ps5/ggml/build.sh
#
# Needs, outside the repo (AGENTS.md):
#   $SPLAT_DATA_DIR/ref/radv-ps5/{lib,include,sdk}   from platform/ps5/radv
#   host: cmake, ninja, glslc (brew install shaderc), SPIRV-Headers
#
# The SDK's clang wrapper finds the real clang through LLVM_CONFIG; on this Mac
# that is Homebrew's llvm@21.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/../../.." && pwd)"

: "${SPLAT_DATA_DIR:=${repo%/*}}"
: "${BUILD_DIR:=$repo/build-ps5-ggml}"
: "${RADV_PS5:=$SPLAT_DATA_DIR/ref/radv-ps5}"
: "${PS5_PAYLOAD_SDK:=$RADV_PS5/sdk}"
if [ -z "${LLVM_CONFIG:-}" ]; then
    for candidate in /opt/homebrew/opt/llvm@21/bin/llvm-config /opt/homebrew/opt/llvm/bin/llvm-config; do
        if [ -x "$candidate" ]; then LLVM_CONFIG=$candidate; break; fi
    done
fi
if [ -z "${LLVM_CONFIG:-}" ] || [ ! -x "$LLVM_CONFIG" ]; then
    echo "build.sh: set LLVM_CONFIG to llvm-config (brew install llvm@21)" >&2
    exit 2
fi
export SPLAT_DATA_DIR RADV_PS5 PS5_PAYLOAD_SDK LLVM_CONFIG

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$BUILD_DIR"
fi

jobs="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
# Keep the machine usable: 8 jobs is the sweet spot on this 16 GiB host.
[ "$jobs" -gt 8 ] && jobs=8

cmake -S "$here" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$here/ps5-toolchain.cmake" \
      -DSPLAT_REPO="$repo"
cmake --build "$BUILD_DIR" -j "$jobs"

echo
echo "built:"
ls -l "$BUILD_DIR"/ggml/src/libggml*.a "$BUILD_DIR"/libsplat_*.a 2>/dev/null || true
ls -l "$BUILD_DIR"/shaders/*.spv 2>/dev/null | sed -n '1,6p' || true
