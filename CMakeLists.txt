cmake_minimum_required(VERSION 3.24)

project(ps5_splat_vr VERSION 0.1.0 LANGUAGES C CXX)

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

if(SPLAT_BUILD_DESKTOP AND NOT SPLAT_HAVE_VULKAN)
    message(WARNING "Vulkan not available: splat_core, the tests and the CLIs build with the CPU "
                    "backend; the desktop viewer is skipped")
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
