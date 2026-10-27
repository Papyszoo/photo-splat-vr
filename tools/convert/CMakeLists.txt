# C1 converter CLI. The top-level CMakeLists adds this directory from the
# desktop block. The miniz static library lives in core/CMakeLists.txt, because
# the checkpoint zip reader in core/model needs it on every build.
add_executable(splat_convert main.cpp)
target_link_libraries(splat_convert PRIVATE splat_core)
