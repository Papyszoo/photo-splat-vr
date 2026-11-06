cmake_minimum_required(VERSION 3.24)

project(ps5_splat_vr VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SPLAT_BUILD_DESKTOP "Build the GLFW + Vulkan desktop target" ON)
option(SPLAT_BUILD_TESTS "Build the doctest suite" ON)
# Detection is automatic; turn this off to exercise the CPU-only path on a host
# that does have Vulkan (CI does exactly that with -DSPLAT_USE_VULKAN=OFF).
option(SPLAT_USE_VULKAN "Use Vulkan when it is found" ON)
# Diagnostic harnesses that are not part of the app (tools/op-audit).
option(SPLAT_BUILD_OP_AUDIT "Build the manual ggml op-audit harnesses" OFF)

# Homebrew is the normal toolchain source on macOS and CMake does not search
# /opt/homebrew by default.
if(APPLE)
    foreach(prefix /opt/homebrew /usr/local)
        if(EXISTS "${prefix}" AND NOT CMAKE_PREFIX_PATH MATCHES "(^|;)${prefix}(;|$)")
            list(APPEND CMAKE_PREFIX_PATH "${prefix}")
        endif()
    endforeach()
endif()

if(SPLAT_USE_VULKAN)
    find_package(Vulkan QUIET)
endif()

# ggml: Vulkan backend when the headers/loader/shaderc are available, CPU
# otherwise. SPLAT_HAVE_VULKAN gates our own Vulkan renderer sources.
set(SPLAT_HAVE_VULKAN OFF)
if(Vulkan_FOUND)
    set(SPLAT_HAVE_VULKAN ON)
endif()

set(GGML_VULKAN ${SPLAT_HAVE_VULKAN} CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
# The desktop path is Vulkan; keep the Metal backend out of the build.
set(GGML_METAL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ggml EXCLUDE_FROM_ALL)

# --- image codecs: WebP and TIFF (L1-B1) -----------------------------------
# Both are CMake subprojects with everything the app does not decode turned
# off: WebP lossy/lossless/alpha, TIFF baseline (none/LZW/PackBits). The
# libraries are linked into splat_core only when their targets exist (see
# core/CMakeLists.txt), so a tree without the submodules still configures.
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
add_subdirectory(third_party/libwebp EXCLUDE_FROM_ALL)

# The SDK has no zlib and we do not ship one, so libtiff is built without every
# optional codec: a deflate or JPEG TIFF is refused by name at decode time
# (core/image/tiff_decoder.cpp) instead of pulling a whole compression stack.
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
add_subdirectory(third_party/libtiff EXCLUDE_FROM_ALL)
# One known archive path for the PS5 title link (platform/ps5/viewer/Makefile
# names it in TITLE_EXTRA_LIBS); libtiff's own target lives in a subdirectory.
if(TARGET tiff)
    set_target_properties(tiff PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libtiff")
endif()

# --- HEIF/AVIF through libheif (L1-B2) -------------------------------------
# third_party/build_codecs.sh installs libheif, libde265 and libaom into one
# prefix, which the gate hands us as CMAKE_PREFIX_PATH. The config package is
# asked for first; the archives are linked by path either way (the installed
# libheif-config.cmake names AOM::aom and the absolute paths of the machine that
# built it, which is exactly wrong for the cross build). Missing prefix: the
# build still configures and HEIC/AVIF report "support is not built".
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
find_path(LIBHEIF_INCLUDE_DIR NAMES libheif/heif.h HINTS "${CODECS_PREFIX}/include")
find_library(LIBHEIF_LIBRARY NAMES heif HINTS "${CODECS_PREFIX}/lib")
find_library(LIBDE265_LIBRARY NAMES de265 HINTS "${CODECS_PREFIX}/lib")
find_library(LIBAOM_LIBRARY NAMES aom HINTS "${CODECS_PREFIX}/lib")
if(LIBHEIF_INCLUDE_DIR AND LIBHEIF_LIBRARY AND LIBDE265_LIBRARY AND LIBAOM_LIBRARY)
    set(SPLAT_HAVE_HEIF ON)
    message(STATUS "HEIF/AVIF decoding: ${LIBHEIF_LIBRARY}")
else()
    message(STATUS "HEIF/AVIF decoding: off (build third_party/build_codecs.sh to enable)")
endif()

if(SPLAT_BUILD_DESKTOP AND NOT SPLAT_HAVE_VULKAN)
    message(WARNING "Vulkan not available: splat_core, the tests and the CLIs build with the CPU "
                    "backend; the desktop viewer is skipped")
endif()

# --- SMB through libsmb2 (L3-B) --------------------------------------------
# A CMake subproject with the examples, Kerberos and GSSAPI off: we use NTLMv2
# only, which is what macOS File Sharing and a Samba/Windows NAS accept.
# `smb2` is its static library; the archive path is pinned so the PS5 title
# link (platform/ps5/viewer/Makefile) can name the same file.
# libsmb2's CMakeLists sets these through option() under the 3.5 policy
# rules, where a plain variable would be replaced by the cache default: they
# are cache entries, forced, or a macOS build finds Kerberos and the link fails.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libsmb2/CMakeLists.txt")
    add_subdirectory(third_party/libsmb2 EXCLUDE_FROM_ALL)
    if(TARGET smb2)
        set_target_properties(smb2 PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libsmb2")
    endif()
endif()

# --- NFS through libnfs (L4) ------------------------------------------------
# The same CMake subproject on both graphs: static, no TLS (RPC-with-TLS needs
# kTLS and gnutls, neither of which the payload has), no tools, no examples, no
# tests, no documentation and no libnfs-side threading (we serialise one
# context per adapter under its own mutex). NFSv4.2 stays on, but a location
# selects 3 or 4 per share.
# libnfs's `nfs` target pins OUTPUT_NAME "libnfs" with PREFIX "" (so the shared
# object is libnfs.so, not liblibnfs.so), which makes the static archive
# libnfs.a - the name the viewer Makefile links. Nothing to override.
set(ENABLE_TLS OFF CACHE STRING "" FORCE)
set(ENABLE_NFS4_2 ON CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_UTILS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(ENABLE_MULTITHREADING OFF CACHE BOOL "" FORCE)
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libnfs/CMakeLists.txt")
    add_subdirectory(third_party/libnfs EXCLUDE_FROM_ALL)
    if(TARGET nfs)
        set_target_properties(nfs PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/third_party/libnfs")
        # libnfs's CMakeLists adds its include paths directory-wide, so
        # <nfsc/libnfs.h> would not reach our sources through the link alone.
        # BUILD_INTERFACE: libnfs exports its target, and an export set refuses a
        # PUBLIC include path that points into the source tree.
        target_include_directories(nfs PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/libnfs/include>")
    endif()
endif()

add_subdirectory(core)

if(SPLAT_BUILD_DESKTOP AND SPLAT_HAVE_VULKAN)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
    add_subdirectory(platform/desktop)
endif()

# The converter and the fallback CLI only need splat_core, which builds with
# the CPU backend alone.
if(SPLAT_BUILD_DESKTOP)
    add_subdirectory(tools/convert)
    add_subdirectory(tools/reconstruct)
    add_subdirectory(tools/depth_scene)
    add_subdirectory(tools/ingest_server)
    add_subdirectory(tools/model_cli)
    add_subdirectory(tools/ui_preview)
endif()

if(SPLAT_BUILD_OP_AUDIT)
    add_subdirectory(tools/op-audit)
endif()

enable_testing()

if(SPLAT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
