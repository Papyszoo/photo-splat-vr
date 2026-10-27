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
# core/model is the C1 model manager and its converter (checkpoint/unpickler,
# zip via the vendored miniz, GGUF writer, licence, SHA-256, DA2 profile). W21
# wired the real models service into the viewer, so it is in the archive; the
# benches name only sha256.cpp, so a static link pulls only that for them.
set(SPLAT_CORE_DIRS
    "${SPLAT_REPO}/core/common"
    "${SPLAT_REPO}/core/scene"
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
target_compile_definitions(splat_core_ps5 PUBLIC
    SPLAT_HAVE_VULKAN=1
    SPLAT_VERSION="0.1.0")

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
