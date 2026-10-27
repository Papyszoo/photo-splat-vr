# GLFW loads the Vulkan loader with dlopen("libvulkan.1.dylib"), which does not
# search Homebrew's lib directory. Point it at the loader that CMake found.
if(APPLE AND Vulkan_LIBRARY)
    target_compile_definitions(glfw PRIVATE _GLFW_VULKAN_LIBRARY="${Vulkan_LIBRARY}")
endif()

# app_mode.cpp is the D1 session (LAN page, inbox watcher, queue, runner, phase
# guard, scene playlist). It only needs splat_core; the window and the Vulkan
# objects stay in main.cpp. tests/app_mode_test.cpp compiles the same source so
# the integration test runs without the GLFW/Vulkan shell.
add_executable(splat_desktop
    main.cpp
    app_mode.cpp
)

target_link_libraries(splat_desktop PRIVATE splat_core glfw Vulkan::Vulkan)
target_compile_definitions(splat_desktop PRIVATE SPLAT_SHADER_DIR="${SPLAT_SHADER_DIR}")
add_dependencies(splat_desktop splat_shaders)
