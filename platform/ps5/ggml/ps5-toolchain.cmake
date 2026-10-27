# CMake toolchain: cross-compile the ggml stack and the project's core sources
# for the PS5 payload target (x86-64 Sie/FreeBSD, payload SDK).
#
#   cmake -S platform/ps5/ggml -B build-ps5-ggml -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=platform/ps5/ggml/ps5-toolchain.cmake
#
# Everything external comes from $SPLAT_DATA_DIR (AGENTS.md): the payload SDK
# and the RADV archive that vk_hello links. Nothing here hard-codes a personal
# path, and the repo only holds this glue.
#
# The RADV headers are the ones the archive was built with, and there is no
# Vulkan loader: cmake/vulkan/VulkanConfig.cmake stands in for the SDK package
# so ggml's Vulkan backend can be configured without one. The final ELF link
# (platform/ps5/bench/Makefile) applies RADV's whole-archive recipe, the same
# one platform/ps5/vk_hello/Makefile uses.
# SPDX-License-Identifier: GPL-3.0-or-later

get_filename_component(_splat_repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT DEFINED SPLAT_DATA_DIR)
    if(DEFINED ENV{SPLAT_DATA_DIR})
        set(SPLAT_DATA_DIR "$ENV{SPLAT_DATA_DIR}")
    else()
        get_filename_component(SPLAT_DATA_DIR "${_splat_repo}/.." ABSOLUTE)
    endif()
endif()

if(NOT DEFINED RADV_PS5)
    set(RADV_PS5 "${SPLAT_DATA_DIR}/ref/radv-ps5" CACHE PATH "RADV archive, headers, SDK")
endif()

if(NOT DEFINED PS5_PAYLOAD_SDK)
    if(DEFINED ENV{PS5_PAYLOAD_SDK})
        set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}" CACHE PATH "PS5 payload SDK")
    else()
        set(PS5_PAYLOAD_SDK "${RADV_PS5}/sdk" CACHE PATH "PS5 payload SDK")
    endif()
endif()

if(NOT EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
    message(FATAL_ERROR
        "PS5 payload SDK not found at ${PS5_PAYLOAD_SDK}. Build the RADV side "
        "(platform/ps5/radv/build-in-docker.sh) or set PS5_PAYLOAD_SDK.")
endif()
if(NOT EXISTS "${RADV_PS5}/include/vulkan/vulkan.h")
    message(FATAL_ERROR "RADV Vulkan headers not found under ${RADV_PS5}/include")
endif()

include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")

# ggml-vulkan's shader generator is a host tool (see host-toolchain.cmake); it
# is built by the same configure, so the shaders are compiled on this machine.
#
# Vulkan headers: compile-time only. RADV's include/ ships the C header the
# archive was built against (1.4.354) but not Vulkan-Hpp, which ggml-vulkan
# needs; Vulkan-Hpp hard-asserts that its own version matches the C header it
# includes, so the pair must come from one source. A host Vulkan-Headers set
# (1.4.363 here) is used for the whole ggml-vulkan translation unit: the 1.4
# core ABI and every KHR struct it uses are unchanged between the two, and
# nothing is linked from it -- the entry points come from RADV at final link.
if(NOT DEFINED PS5_VULKAN_HEADERS)
    foreach(_candidate /opt/homebrew/include /usr/local/include "$ENV{VULKAN_SDK}/include")
        if(EXISTS "${_candidate}/vulkan/vulkan.hpp")
            set(PS5_VULKAN_HEADERS "${_candidate}" CACHE PATH "Vulkan headers (host, compile-time)")
            break()
        endif()
    endforeach()
endif()
if(NOT PS5_VULKAN_HEADERS)
    message(FATAL_ERROR
        "Vulkan-Hpp not found: brew install vulkan-headers, or set PS5_VULKAN_HEADERS")
endif()
if(NOT DEFINED Vulkan_DIR)
    set(Vulkan_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake/vulkan" CACHE PATH "stand-in Vulkan package")
endif()
if(NOT DEFINED Vulkan_GLSLC_EXECUTABLE)
    find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc HINTS /opt/homebrew/bin "$ENV{VULKAN_SDK}/bin")
endif()
if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found (brew install shaderc); shaders are generated on the host")
endif()

# SPIRV-Headers is part of the Vulkan-Hpp include set and is required by
# ggml-vulkan's CMakeLists. A host copy is right: only headers are used.
if(NOT DEFINED SPIRV-Headers_DIR)
    foreach(_prefix /opt/homebrew /usr/local /usr)
        if(EXISTS "${_prefix}/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake")
            set(SPIRV-Headers_DIR "${_prefix}/share/cmake/SPIRV-Headers" CACHE PATH "")
            break()
        endif()
    endforeach()
endif()
if(DEFINED ENV{VULKAN_SDK} AND NOT DEFINED SPIRV-Headers_DIR)
    set(SPIRV-Headers_DIR "$ENV{VULKAN_SDK}/share/cmake/SPIRV-Headers" CACHE PATH "")
endif()
if(NOT DEFINED SPIRV-Headers_DIR)
    message(FATAL_ERROR "SPIRV-Headers package not found (brew install spirv-headers)")
endif()

# try_compile links an executable by default, which a payload toolchain cannot
# do without the payload link recipe; a static library is enough for the tests
# CMake makes here.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_INIT   "-O2 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_INIT "-O2 -g -fno-omit-frame-pointer")

# The payload links with libc++ from the SDK (no libstdc++, which would need
# exceptions/RTTI support the target does not have in libc.a).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
