# Manual op-audit harness (Spike A). Off by default: these binaries are only
# built when SPLAT_BUILD_OP_AUDIT=ON, because they are diagnostic one-shots, not
# part of the app. See README.md.
foreach(probe sharp_ops conv_probe conv_probe_direct conv_probe_st mm_probe)
    add_executable(${probe} ${probe}.cpp)
    target_link_libraries(${probe} PRIVATE splat_core)
endforeach()
