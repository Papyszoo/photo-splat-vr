# zstd v1.5.7 (third_party/zstd) ships a CMake project, but it builds the
# compressor, the dictionary builder and the CLI tools as well, and the import
# path only ever decompresses (the spz v4 stream). This list is the decompressor
# and the common code it needs, taken from lib/libzstd.mk's ZSTD_COMMON_FILES
# and ZSTD_DECOMPRESS_FILES. The consuming CMakeLists.txt defines the target
# with ZSTD_LEGACY_SUPPORT=0 (no lib/legacy), ZSTD_MULTITHREAD=0 (no threading
# in a decompress-only library), ZSTD_DISABLE_ASM=1 (the amd64 assembly is not
# in this list and must not be referenced; it would also need the ASM language
# enabled in the desktop project) and ZSTD_TRACE=0 (the trace hooks are weak
# undefined symbols, which the PS5 title link refuses).
#
# SPDX-License-Identifier: GPL-3.0-or-later
file(GLOB SPLAT_ZSTD_SOURCES CONFIGURE_DEPENDS
    "${SPLAT_ZSTD_DIR}/lib/common/*.c"
    "${SPLAT_ZSTD_DIR}/lib/decompress/*.c")
