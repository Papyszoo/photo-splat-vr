# Host toolchain for ggml's vulkan-shaders-gen (ExternalProject).
#
# The shaders are compiled to SPIR-V on the build machine; only the resulting
# uint32_t arrays cross to the console. ggml's CMakeLists passes this file to
# the nested configure when CMAKE_CROSSCOMPILING is set.
# SPDX-License-Identifier: GPL-3.0-or-later

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
