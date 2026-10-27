# External depth map -> C4 scene folder (the fallback's own builder).
#
# This is how a model that is not in this repo (MoGe-2, Depth Anything 3) is
# compared on the product's splat construction: the harness writes a .npy depth
# map, this CLI feeds it to fallback_scene_from_depth and writes the standard
# scene folder (scene.psplat, meta.json, thumb.jpg, original.jpg).
add_executable(splat_depth_scene main.cpp)
target_link_libraries(splat_depth_scene PRIVATE splat_core)
