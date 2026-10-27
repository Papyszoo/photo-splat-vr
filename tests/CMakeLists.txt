# Every tests/*_test.cpp is compiled. Adding a test file needs no CMake edit.
file(GLOB SPLAT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT SPLAT_HAVE_VULKAN)
    list(REMOVE_ITEM SPLAT_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gpu_probe_test.cpp")
endif()

add_executable(splat_tests
    test_main.cpp
    ${SPLAT_TEST_SOURCES}
)

target_link_libraries(splat_tests PRIVATE splat_core)
target_include_directories(splat_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

if(SPLAT_HAVE_VULKAN)
    target_compile_definitions(splat_tests PRIVATE SPLAT_SHADER_DIR="${SPLAT_SHADER_DIR}")
    add_dependencies(splat_tests splat_shaders)
endif()

add_test(NAME splat_tests COMMAND splat_tests)
