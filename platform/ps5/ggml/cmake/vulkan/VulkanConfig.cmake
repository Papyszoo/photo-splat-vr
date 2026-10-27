# Stand-in Vulkan package for the PS5 payload cross build.
#
# RADV is linked directly (libvulkan_radeon.ps5.a, whole archive) by
# platform/ps5/bench/Makefile, exactly as platform/ps5/vk_hello does, and there
# is no Vulkan loader on the console. ggml's Vulkan backend only needs the
# headers at compile time, so Vulkan::Vulkan is an interface target with no
# library and this package never asks for one.
#
# Inputs (cache variables set by ps5-toolchain.cmake):
#   PS5_VULKAN_HEADERS      the RADV build's include/ directory
#   Vulkan_GLSLC_EXECUTABLE host glslc, used for shader generation only
# SPDX-License-Identifier: GPL-3.0-or-later

set(Vulkan_FOUND TRUE)
set(Vulkan_glslc_FOUND TRUE)
set(Vulkan_VERSION "1.4.354")
set(Vulkan_INCLUDE_DIRS "${PS5_VULKAN_HEADERS}")
set(Vulkan_INCLUDE_DIR "${PS5_VULKAN_HEADERS}")

if(NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PS5_VULKAN_HEADERS}")
endif()

# ggml's shader-extension probes shell out to glslc; the configure below passes
# Vulkan_GLSLC_EXECUTABLE through either way.
set(Vulkan_GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}" CACHE FILEPATH "" FORCE)
