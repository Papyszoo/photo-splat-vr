# C4 dev CLI: photo -> scene folder through the C9 job runner.
#
# The default model path points at the weights directory outside the repo
# ($SPLAT_DATA_DIR/weights, see AGENTS.md). Override with --model or
# SPLAT_MODEL_PATH.
add_executable(splat_reconstruct main.cpp)
target_link_libraries(splat_reconstruct PRIVATE splat_core)
target_compile_definitions(splat_reconstruct PRIVATE SPLAT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
