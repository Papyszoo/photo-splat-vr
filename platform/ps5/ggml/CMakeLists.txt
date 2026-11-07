# Cross build of the ggml stack + the project's core sources for the PS5
# payload target. Libraries only: the payloads are linked by
# platform/ps5/bench/Makefile with RADV's whole-archive recipe.
#
#   cmake -S platform/ps5/ggml -B build-ps5-ggml -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=platform/ps5/ggml/ps5-toolchain.cmake
#   cmake --build build-ps5-ggml
#
# What it produces:
#   libggml*.a          ggml 0.27.0 (third_party/ggml), CPU + Vulkan backends
#   libsplat_core_ps5.a core/common, core/scene, core/render (the C6 splat
#                       renderer), core/recon/ggml, core/recon/fallback,
#                       core/recon/sharp, plus the photo decoder
#                       (core/jobs/image_io) the benches need
#   libsplat_miniz.a    the vendored miniz the core links
#   shaders/*.spv       core/render/shaders, compiled for the viewer title
#
# ggml-vulkan is configured with the stand-in Vulkan package in
# cmake/vulkan/VulkanConfig.cmake: headers only, no loader. The shaders are
# generated on this host by ggml's vulkan-shaders-gen and arrive as arrays in
# the archive. The Vulkan entry points come from RADV at final link time.
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)

project(ps5_splat_payload LANGUAGES C CXX ASM)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "configure with -DCMAKE_TOOLCHAIN_FILE=platform/ps5/ggml/ps5-toolchain.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SPLAT_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "repo root")

# ---- ggml ---------------------------------------------------------------
# CPU + Vulkan only. No OpenMP (the payload has no libomp), no native
# -march (the target is fixed: Zen 2, AVX2/FMA/F16C/BMI2, no AVX-512), and no
# tests/examples/tools.
set(GGML_VULKAN ON CACHE BOOL "" FORCE)
set(GGML_CPU ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
set(GGML_AVX512_VBMI OFF CACHE BOOL "" FORCE)
set(GGML_AVX512_VNNI OFF CACHE BOOL "" FORCE)
set(GGML_AVX512_BF16 OFF CACHE BOOL "" FORCE)
set(GGML_AVX_VNNI OFF CACHE BOOL "" FORCE)
set(GGML_AMX_TILE OFF CACHE BOOL "" FORCE)
set(GGML_AMX_INT8 OFF CACHE BOOL "" FORCE)
set(GGML_AMX_BF16 OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_ACCELERATE OFF CACHE BOOL "" FORCE)
set(GGML_LLAMAFILE OFF CACHE BOOL "" FORCE)
set(GGML_SCHED OFF CACHE BOOL "" FORCE)
# The shader generator is a host tool and must not inherit this toolchain.
set(GGML_VULKAN_SHADERS_GEN_TOOLCHAIN
    "${CMAKE_CURRENT_LIST_DIR}/host-toolchain.cmake" CACHE FILEPATH "" FORCE)

add_subdirectory("${SPLAT_REPO}/third_party/ggml" "${CMAKE_BINARY_DIR}/ggml" EXCLUDE_FROM_ALL)

# ---- image codecs: WebP and TIFF (L1-B1) -----------------------------------
# Same two subprojects and the same options as the desktop build
# (CMakeLists.txt), configured here with the cross toolchain. Everything the
# app does not decode is off: the payload never needs the WebP tools and libtiff
# is baseline only, because the SDK has no zlib.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
add_subdirectory("${SPLAT_REPO}/third_party/libwebp" "third_party/libwebp" EXCLUDE_FROM_ALL)

set(tiff-static ON CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-cxx OFF CACHE BOOL "" FORCE)
set(tiff-install OFF CACHE BOOL "" FORCE)
set(jpeg OFF CACHE BOOL "" FORCE)
set(zlib OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
add_subdirectory("${SPLAT_REPO}/third_party/libtiff" "third_party/libtiff" EXCLUDE_FROM_ALL)
# The viewer Makefile links $(BUILD)/third_party/libtiff/libtiff.a; libtiff's
# own target lives one directory deeper than that.
if(TARGET tiff)
    set_target_properties(tiff PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libtiff")
endif()

# ---- HEIF/AVIF through libheif (L1-B2) -------------------------------------
# The same find_package with CMAKE_PREFIX_PATH=$CODECS_PREFIX (build.sh appends
# it from the environment). The archives are linked by path: the installed
# libheif-config.cmake names AOM::aom and the absolute paths of the host that
# built it, which a cross link must not see.
set(CODECS_PREFIX "$ENV{CODECS_PREFIX}" CACHE PATH "codec install prefix (build_codecs.sh)")
if(CODECS_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${CODECS_PREFIX}")
endif()
set(SPLAT_HAVE_HEIF OFF)
# A build tree configured before the codec prefix existed caches NOTFOUND for
# every one of these; drop those entries so a fresh prefix is picked up.
foreach(_heif_var LIBHEIF_INCLUDE_DIR LIBHEIF_LIBRARY LIBDE265_LIBRARY LIBAOM_LIBRARY)
    if(DEFINED ${_heif_var} AND "${${_heif_var}}" MATCHES "-NOTFOUND$")
        unset(${_heif_var} CACHE)
    endif()
endforeach()
find_package(heif CONFIG QUIET)
# The payload toolchain sets CMAKE_FIND_ROOT_PATH_MODE_{LIBRARY,INCLUDE} to
# ONLY, which re-roots even an absolute HINTS path, so a host prefix is never
# searched. These lookups name the prefix build_codecs.sh installed into and
# keep the default paths out of a cross link.
if(CODECS_PREFIX)
    find_path(LIBHEIF_INCLUDE_DIR NAMES libheif/heif.h
              HINTS "${CODECS_PREFIX}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LIBHEIF_LIBRARY NAMES heif
                 HINTS "${CODECS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LIBDE265_LIBRARY NAMES de265
                 HINTS "${CODECS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    find_library(LIBAOM_LIBRARY NAMES aom
                 HINTS "${CODECS_PREFIX}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
if(LIBHEIF_INCLUDE_DIR AND LIBHEIF_LIBRARY AND LIBDE265_LIBRARY AND LIBAOM_LIBRARY)
    set(SPLAT_HAVE_HEIF ON)
    message(STATUS "HEIF/AVIF decoding: ${LIBHEIF_LIBRARY}")
else()
    # A prefix the gate handed us but could not use is a broken cross build,
    # not a tree that simply has no codecs: fail instead of shipping a stub.
    if(CODECS_PREFIX)
        message(FATAL_ERROR
            "CODECS_PREFIX=${CODECS_PREFIX} but libheif/libde265/libaom were not found "
            "under it (expected ${CODECS_PREFIX}/lib/libheif.a)")
    endif()
    message(STATUS "HEIF/AVIF decoding: off (set CODECS_PREFIX to the codec prefix)")
endif()

# --- SMB through libsmb2 (L3-B) ---------------------------------------------
# The same subproject and options as the desktop build. No Kerberos and no
# GSSAPI: NTLMv2 only, and the payload SDK has no libkrb5. The archive is
# pinned to $(BUILD)/third_party/libsmb2/libsmb2.a for the title link.
# libsmb2's CMakeLists sets these through option() under the 3.5 policy
# rules, where a plain variable would be replaced by the cache default: they
# are cache entries, forced, or a macOS build finds Kerberos and the link fails.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
if(EXISTS "${SPLAT_REPO}/third_party/libsmb2/CMakeLists.txt")
    add_subdirectory("${SPLAT_REPO}/third_party/libsmb2" "third_party/libsmb2"
                     EXCLUDE_FROM_ALL)
    if(TARGET smb2)
        set_target_properties(smb2 PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libsmb2")
    endif()
endif()

# --- NFS through libnfs (L4) ------------------------------------------------
# The same subproject and options as the desktop build, configured with the
# cross toolchain. The archive is pinned to
# $(BUILD)/third_party/libnfs/libnfs.a for the title link.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TLS OFF CACHE STRING "" FORCE)
set(ENABLE_NFS4_2 ON CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_UTILS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(ENABLE_MULTITHREADING OFF CACHE BOOL "" FORCE)
if(EXISTS "${SPLAT_REPO}/third_party/libnfs/CMakeLists.txt")
    add_subdirectory("${SPLAT_REPO}/third_party/libnfs" "third_party/libnfs"
                     EXCLUDE_FROM_ALL)
    if(TARGET nfs)
        # libnfs pins OUTPUT_NAME "libnfs" with PREFIX "", so the static
        # archive is libnfs.a - the name the viewer Makefile links.
        set_target_properties(nfs PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libnfs")
        # libnfs's CMakeLists adds its include paths directory-wide, so
        # <nfsc/libnfs.h> would not reach our sources through the link alone.
        target_include_directories(nfs PUBLIC
            "$<BUILD_INTERFACE:${SPLAT_REPO}/third_party/libnfs/include>")
        # The SDK's libc exports neither getprotobyname() nor getservbyport(),
        # and libnfs's FreeBSD branch calls both from lib/socket.c;
        # libnfs_compat.cpp supplies them for the cross build only.
        target_sources(nfs PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libnfs_compat.cpp")
    endif()
endif()

# ---- project core ---------------------------------------------------------
# The directories the payloads need. Deliberately enumerated instead of a
# glob over core/: core/ingest and core/model's converter (zip/zlib/http) are
# not part of a reconstruction payload. core/jobs is the queue/runner and the
# photo decoder (stb_image): the benches use image_io, and the viewer's queue
# screen names the runner's scene-folder helper while W19's ps5_jobs.cpp drives
# the queue and the runner for real, so the whole (small) component is in the
# archive; a static link pulls only the objects a payload references.
# core/render is the C6 splat renderer the viewer title (platform/ps5/viewer)
# draws with; the benches do not reference it, so a static link pulls none of it.
# core/gallery, core/ui (canvas, input, UiApp, fakes, services), core/ui/screens
# and core/capture are the panel UI the TV viewer boots into (W15); the benches
# do not reference them either, so they cost the payloads nothing.
# core/splat_import is the splat file readers and the import thinning (L2);
# core/scene's read_scene_path dispatches through it.
# core/model is the C1 model manager and its converter (checkpoint/unpickler,
# zip via the vendored miniz, GGUF writer, licence, SHA-256, DA2 profile). W21
# wired the real models service into the viewer, so it is in the archive; the
# benches name only sha256.cpp, so a static link pulls only that for them.
set(SPLAT_CORE_DIRS
    "${SPLAT_REPO}/core/common"
    "${SPLAT_REPO}/core/vfs"
    "${SPLAT_REPO}/core/image"
    "${SPLAT_REPO}/core/scene"
    "${SPLAT_REPO}/core/splat_import"
    "${SPLAT_REPO}/core/render"
    "${SPLAT_REPO}/core/gallery"
    "${SPLAT_REPO}/core/ui"
    "${SPLAT_REPO}/core/ui/screens"
    "${SPLAT_REPO}/core/capture"
    "${SPLAT_REPO}/core/jobs"
    "${SPLAT_REPO}/core/model"
    "${SPLAT_REPO}/core/recon/ggml"
    "${SPLAT_REPO}/core/recon/fallback"
    "${SPLAT_REPO}/core/recon/sharp")

set(SPLAT_CORE_SOURCES "")
foreach(dir IN LISTS SPLAT_CORE_DIRS)
    file(GLOB _sources CONFIGURE_DEPENDS "${dir}/*.cpp")
    list(APPEND SPLAT_CORE_SOURCES ${_sources})
endforeach()

# W21 removed the two filters this list used to need: core/ui/model_service.cpp
# (the C1 ModelManager adapter) is linked again now that core/model is in the
# archive and the viewer provides the real chain, and core/model/sha256.cpp comes
# in with its own directory instead of being appended by name.

add_library(splat_core_ps5 STATIC ${SPLAT_CORE_SOURCES})
# The panel font, compiled in like the desktop build (core/CMakeLists.txt).
include("${SPLAT_REPO}/cmake/embed_file.cmake")
splat_embed_file(splat_core_ps5 "${SPLAT_REPO}/third_party/inter/Inter-Medium.ttf" kUiFontData)
target_include_directories(splat_core_ps5 PUBLIC
    "${SPLAT_REPO}/core"
    "${SPLAT_REPO}/third_party"
    "${SPLAT_REPO}/third_party/json")
# core/render includes <vulkan/vulkan.h>. The C header is the one the RADV
# archive was built against; PRIVATE, so it cannot shadow the host header set
# ggml-vulkan is compiled with (PS5_VULKAN_HEADERS).
target_include_directories(splat_core_ps5 PRIVATE "${RADV_PS5}/include")
target_link_libraries(splat_core_ps5 PUBLIC ggml splat_miniz)
# The image codecs, when their subprojects configured (the same guard as
# core/CMakeLists.txt). The title link needs the archives too; the viewer
# Makefile names them in TITLE_EXTRA_LIBS.
if(TARGET webp)
    target_link_libraries(splat_core_ps5 PUBLIC webp)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_WEBP=1)
endif()
if(TARGET tiff)
    target_link_libraries(splat_core_ps5 PUBLIC tiff)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_TIFF=1)
endif()
if(SPLAT_HAVE_HEIF)
    target_include_directories(splat_core_ps5 PUBLIC "${LIBHEIF_INCLUDE_DIR}")
    target_link_libraries(splat_core_ps5 PUBLIC "${LIBHEIF_LIBRARY}" "${LIBDE265_LIBRARY}"
                                               "${LIBAOM_LIBRARY}")
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_HEIF=1)
endif()

# SMB through libsmb2 (L3-B): the same guard as the desktop build
# (core/CMakeLists.txt). The title link names the archive as well.
if(TARGET smb2)
    target_link_libraries(splat_core_ps5 PUBLIC smb2)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_SMB2=1)
    # FreeBSD's libc has no SOL_TCP (a Linux name): without it socket.c calls
    # getprotobyname(), which the payload SDK does not export, and the title
    # link fails. TCP's option level is IPPROTO_TCP on both systems, so the
    # library's `setsockopt` path takes the intended branch unchanged.
    target_compile_definitions(smb2 PRIVATE SOL_TCP=IPPROTO_TCP)
endif()

# NFS through libnfs (L4): the same guard as the desktop build. The archive is
# named in the title link as well.
if(TARGET nfs)
    target_link_libraries(splat_core_ps5 PUBLIC nfs)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_NFS=1)
endif()

# RAW through LibRaw (L1-C): the same explicit source list the desktop build
# uses (third_party/libraw_sources.cmake), with -DLIBRAW_NOTHREADS and without
# USE_ZLIB/USE_JPEG/USE_LCMS. The viewer title links libsplat_libraw.a.
# libraw_compat.cpp supplies memmem, which the SDK's libc.a does not export.
if(EXISTS "${SPLAT_REPO}/third_party/libraw/buildfiles/libraw.pro")
    set(SPLAT_LIBRAW_DIR "${SPLAT_REPO}/third_party/libraw")
    include("${SPLAT_REPO}/third_party/libraw_sources.cmake")
    add_library(splat_libraw STATIC ${SPLAT_LIBRAW_SOURCES}
                "${CMAKE_CURRENT_LIST_DIR}/libraw_compat.cpp")
    target_include_directories(splat_libraw PUBLIC "${SPLAT_LIBRAW_DIR}")
    target_compile_definitions(splat_libraw PUBLIC LIBRAW_NOTHREADS)
    target_link_libraries(splat_core_ps5 PUBLIC splat_libraw)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_RAW=1)
endif()
target_compile_definitions(splat_core_ps5 PUBLIC
    SPLAT_HAVE_VULKAN=1
    SPLAT_VERSION="0.6.0")

# ---- the renderer's shaders ------------------------------------------------
# core/render loads six SPIR-V modules by name at run time (RendererOptions::
# shader_dir); the viewer title ships them under /app0/shaders. Compiled here by
# the host's glslc, the same flags core/CMakeLists.txt uses for the desktop
# build, so a console frame runs the same shaders as a screenshot.
set(SPLAT_SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
file(GLOB SPLAT_SHADER_SOURCES CONFIGURE_DEPENDS
    "${SPLAT_REPO}/core/render/shaders/*.comp"
    "${SPLAT_REPO}/core/render/shaders/*.vert"
    "${SPLAT_REPO}/core/render/shaders/*.frag")
set(SPLAT_SPIRV_OUTPUTS "")
foreach(shader IN LISTS SPLAT_SHADER_SOURCES)
    get_filename_component(shader_name "${shader}" NAME)
    set(spirv "${SPLAT_SHADER_DIR}/${shader_name}.spv")
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPLAT_SHADER_DIR}"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O -o "${spirv}" "${shader}"
        DEPENDS "${shader}"
        COMMENT "glslc ${shader_name}"
        VERBATIM)
    list(APPEND SPLAT_SPIRV_OUTPUTS "${spirv}")
endforeach()
add_custom_target(splat_shaders_ps5 ALL DEPENDS ${SPLAT_SPIRV_OUTPUTS})

add_library(splat_miniz STATIC
    "${SPLAT_REPO}/third_party/miniz/miniz.c"
    "${SPLAT_REPO}/third_party/miniz/miniz_tdef.c"
    "${SPLAT_REPO}/third_party/miniz/miniz_tinfl.c"
    "${SPLAT_REPO}/third_party/miniz/miniz_zip.c")
target_include_directories(splat_miniz PUBLIC
    "${SPLAT_REPO}/core/common"
    "${SPLAT_REPO}/third_party/miniz")

# zstd, decompression only (L2-A): the same sources and defines as the desktop
# build (core/CMakeLists.txt). spz v4 needs it; nothing here compresses.
if(EXISTS "${SPLAT_REPO}/third_party/zstd/lib/zstd.h")
    set(SPLAT_ZSTD_DIR "${SPLAT_REPO}/third_party/zstd")
    include("${SPLAT_REPO}/third_party/zstd_sources.cmake")
    add_library(splat_zstd STATIC ${SPLAT_ZSTD_SOURCES})
    target_include_directories(splat_zstd PUBLIC "${SPLAT_ZSTD_DIR}/lib")
    target_compile_definitions(splat_zstd PUBLIC
        ZSTD_LEGACY_SUPPORT=0
        ZSTD_MULTITHREAD=0
        ZSTD_DISABLE_ASM=1
        # The tracing hooks are weak undefined symbols; a PS5 title may export
        # nothing, and the console link refuses them.
        ZSTD_TRACE=0)
    target_link_libraries(splat_core_ps5 PUBLIC splat_zstd)
    target_compile_definitions(splat_core_ps5 PUBLIC SPLAT_HAVE_ZSTD=1)
endif()

# The payload provides the Vulkan entry points RADV's archive does not export
# by itself; see ps5_vulkan.cpp. vk_reset.cpp releases ggml-vulkan's cached
# VkDevice between op_audit's coopmat passes (see its header).
add_library(splat_ps5_glue STATIC ps5_vulkan.cpp bench_env.cpp vk_reset.cpp)
target_include_directories(splat_ps5_glue PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}"
    "${SPLAT_REPO}/third_party/ggml/src"
    "${SPLAT_REPO}/third_party/ggml/src/ggml-vulkan"
    "${CMAKE_BINARY_DIR}/ggml/src/ggml-vulkan"
    "${SPLAT_REPO}/third_party/ggml/include"
    "${PS5_VULKAN_HEADERS}"
    "${PS5_PAYLOAD_SDK}/target/include")
# vk_reset.cpp includes ggml-vulkan's internal types header, which includes the
# generated shader header; build the backend's generated files first.
add_dependencies(splat_ps5_glue ggml-vulkan)
