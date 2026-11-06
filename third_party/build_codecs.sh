#!/usr/bin/env bash
# Build and install the HEIF/AVIF decoder stack (libde265, libaom, libheif)
# into one prefix, so both the desktop build and the PS5 cross build can find
# it with CMAKE_PREFIX_PATH.
#
#   third_party/build_codecs.sh --build-dir <dir> --prefix <dir> \
#       --toolchain <file|none> --aom-cpu <x86_64|generic> [--clean]
#
# Crates do not exist for these targets: libaom is decode-only, libheif is
# built without plugin loading (the decoders are linked in), and libde265 is
# the HEVC decoder behind HEIC. Idempotent: once <prefix>/lib/libheif.a exists
# this exits 0 immediately, which is what makes a second gate run cheap.
#
# --aom-cpu names the target CPU. A native build (--toolchain none) always uses
# the host's own architecture: passing x86_64 for a native build on Apple
# Silicon would ask libaom for x86 SIMD assembly this host cannot run, so the
# request is mapped to the host and reported. A cross build passes it through.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$here/.." && pwd)"

BUILD_DIR=""
PREFIX=""
TOOLCHAIN="none"
AOM_CPU=""
CLEAN=0

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --prefix) PREFIX="${2:-}"; shift 2 ;;
        --toolchain) TOOLCHAIN="${2:-}"; shift 2 ;;
        --aom-cpu) AOM_CPU="${2:-}"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage ;;
        *) echo "build_codecs.sh: unknown argument '$1'" >&2; usage ;;
    esac
done

[ -n "$BUILD_DIR" ] || { echo "build_codecs.sh: --build-dir is required" >&2; exit 2; }
[ -n "$PREFIX" ] || { echo "build_codecs.sh: --prefix is required" >&2; exit 2; }
[ -n "$AOM_CPU" ] || AOM_CPU=generic

for library in libde265 libaom libheif; do
    [ -f "$repo/third_party/$library/CMakeLists.txt" ] || {
        echo "build_codecs.sh: third_party/$library is missing (git submodule update --init)" >&2
        exit 2
    }
done

# The prefix is only usable when every archive is there *and* libheif really
# carries both decoder backends: a libheif configured "not found" is still a
# valid archive that decodes nothing (the PS5 title shipped HEIC/AVIF as "not
# built in" until the console pass caught it). This runs on the warm path too,
# so a prefix built before the explicit LIBDE265_LIBRARY/AOM_LIBRARY fix cannot
# pass the gate just because the file exists.
verify_prefix() {
    local archive symbol symbols
    for archive in libheif.a libde265.a libaom.a; do
        [ -f "$PREFIX/lib/$archive" ] || {
            echo "build_codecs.sh: $PREFIX/lib/$archive was not installed" >&2
            return 1
        }
    done
    # One nm run, then grep the captured text: `nm ... | grep -q` makes grep exit
    # at the first match, nm dies of SIGPIPE, and `set -o pipefail` turns a
    # present backend into "missing" (that is how the check passed as a no-op
    # for a whole stage).
    symbols="$(nm "$PREFIX/lib/libheif.a" 2>/dev/null)" || {
        echo "build_codecs.sh: cannot read $PREFIX/lib/libheif.a" >&2
        return 1
    }
    for symbol in get_decoder_plugin_libde265 get_decoder_plugin_aom; do
        case "$symbols" in
            *"$symbol"*) ;;
            *)
                echo "build_codecs.sh: $PREFIX/lib/libheif.a has no $symbol backend" >&2
                return 1
                ;;
        esac
    done
    return 0
}

if [ -f "$PREFIX/lib/libheif.a" ]; then
    if verify_prefix; then
        echo "build_codecs: $PREFIX already has libheif.a with both decoder backends"
        exit 0
    fi
    echo "build_codecs: $PREFIX/lib/libheif.a is incomplete; rebuilding into the same prefix"
fi

TOOLCHAIN_ARG=""
case "$TOOLCHAIN" in
    none|"") ;;
    *) [ -f "$TOOLCHAIN" ] || { echo "build_codecs.sh: no toolchain file $TOOLCHAIN" >&2; exit 2; }
       TOOLCHAIN_ARG="-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN" ;;
esac

host_arch="$(uname -m)"
if [ -z "$TOOLCHAIN_ARG" ] && [ "$AOM_CPU" != "generic" ] && [ "$AOM_CPU" != "$host_arch" ]; then
    echo "build_codecs: native build on $host_arch, so AOM_TARGET_CPU=$host_arch (asked for $AOM_CPU)"
    AOM_CPU="$host_arch"
fi

if [ "$CLEAN" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
[ "$jobs" -gt 8 ] && jobs=8

mkdir -p "$BUILD_DIR" "$PREFIX"

# configure_build_install <name> <source-dir> <cmake args...>
configure_build_install() {
    local name="$1"
    local source="$2"
    shift 2
    echo "build_codecs: $name"
    # shellcheck disable=SC2086  # TOOLCHAIN_ARG is one optional argument
    cmake -S "$source" -B "$BUILD_DIR/$name" -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          $TOOLCHAIN_ARG "$@" >"$BUILD_DIR/$name-configure.log" 2>&1 || {
        tail -n 30 "$BUILD_DIR/$name-configure.log" >&2
        echo "build_codecs.sh: $name failed to configure" >&2
        exit 1
    }
    cmake --build "$BUILD_DIR/$name" -j "$jobs" >"$BUILD_DIR/$name-build.log" 2>&1 || {
        tail -n 40 "$BUILD_DIR/$name-build.log" >&2
        echo "build_codecs.sh: $name failed to build" >&2
        exit 1
    }
    cmake --install "$BUILD_DIR/$name" >"$BUILD_DIR/$name-install.log" 2>&1 || {
        tail -n 20 "$BUILD_DIR/$name-install.log" >&2
        echo "build_codecs.sh: $name failed to install" >&2
        exit 1
    }
}

# HEVC decoder for HEIC. The SDL player and the dec265 example tool are not
# part of a decoder library (libde265 1.1.3 spells the latter ENABLE_DECODER;
# the "dec265" directory only builds when it is on, and it links -lm, which the
# payload SDK does not have).
configure_build_install libde265 "$repo/third_party/libde265" \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_SDL=OFF \
    -DENABLE_DECODER=OFF \
    -DENABLE_ENCODER=OFF

# AV1 decoder for AVIF. Decode only: no encoder, no tests, no examples, no
# tools, and no assembly for a target we are not compiling for.
configure_build_install libaom "$repo/third_party/libaom" \
    -DBUILD_SHARED_LIBS=OFF \
    -DCONFIG_AV1_ENCODER=0 \
    -DAOM_TARGET_CPU="$AOM_CPU" \
    -DENABLE_TESTS=0 \
    -DENABLE_EXAMPLES=0 \
    -DENABLE_APPS=0 \
    -DENABLE_TOOLS=0 \
    -DENABLE_DOCS=0 \
    -DENABLE_TESTDATA=0 \
    -DCONFIG_PIC=1

# The container: HEIC/HEIF through libde265, AVIF through libaom, everything
# else off (no encoders, no plugins, no sharpyuv, no uncompressed codec).
# FindLIBDE265/FindAOM do not search a cross target's CMAKE_PREFIX_PATH, so the
# headers and archives are named explicitly: without them libheif configures
# "not found" and still builds an archive that decodes nothing (the PS5 title
# shipped HEIC/AVIF as "not built in" until the console pass caught it).
configure_build_install libheif "$repo/third_party/libheif" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DLIBDE265_INCLUDE_DIR="$PREFIX/include" \
    -DLIBDE265_LIBRARY="$PREFIX/lib/libde265.a" \
    -DAOM_INCLUDE_DIR="$PREFIX/include" \
    -DAOM_LIBRARY="$PREFIX/lib/libaom.a" \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_PLUGIN_LOADING=OFF \
    -DWITH_LIBDE265=ON \
    -DWITH_AOM_DECODER=ON \
    -DWITH_AOM_ENCODER=OFF \
    -DWITH_X265=OFF \
    -DWITH_X264=OFF \
    -DWITH_OpenH264_DECODER=OFF \
    -DWITH_KVAZAAR=OFF \
    -DWITH_UVG266=OFF \
    -DWITH_VVDEC=OFF \
    -DWITH_VVENC=OFF \
    -DWITH_SvtEnc=OFF \
    -DWITH_DAV1D=OFF \
    -DWITH_RAV1E=OFF \
    -DWITH_JPEG_DECODER=OFF \
    -DWITH_JPEG_ENCODER=OFF \
    -DWITH_OpenJPEG_ENCODER=OFF \
    -DWITH_OpenJPEG_DECODER=OFF \
    -DWITH_OPENJPH_ENCODER=OFF \
    -DWITH_OPENJPH_DECODER=OFF \
    -DWITH_FFMPEG_DECODER=OFF \
    -DWITH_UNCOMPRESSED_CODEC=OFF \
    -DWITH_EXAMPLES=OFF \
    -DWITH_EXAMPLE_HEIF_THUMB=OFF \
    -DWITH_EXAMPLE_HEIF_VIEW=OFF \
    -DWITH_GDK_PIXBUF=OFF \
    -DWITH_LIBSHARPYUV=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_DOCUMENTATION=OFF

for archive in libheif.a libde265.a libaom.a; do
    [ -f "$PREFIX/lib/$archive" ] || {
        echo "build_codecs.sh: $PREFIX/lib/$archive was not installed" >&2
        exit 1
    }
done
# A libheif without its decoder backends is still a valid archive: check the
# backends are really in it, not just that the file exists (verify_prefix, so
# the warm path and this one use one rule).
verify_prefix || exit 1
echo "build_codecs: installed $(ls "$PREFIX/lib"/*.a | wc -l | tr -d ' ') archives in $PREFIX"
