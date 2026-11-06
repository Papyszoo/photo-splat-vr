# Every tests/*_test.cpp is compiled. Adding a test file needs no CMake edit.
file(GLOB SPLAT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

if(NOT SPLAT_HAVE_VULKAN)
    list(REMOVE_ITEM SPLAT_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gpu_probe_test.cpp")
endif()

# resolver_shim.cpp defines getaddrinfo()/freeaddrinfo() for this binary: the
# title DEFINES the same two names in platform/ps5/net/resolve_glue.cpp (the
# payload's libc exports them, so a definition in the title wins over the SDK
# archive; stage T replaced the earlier --wrap), and the definition here binds
# the references from the libsmb2/libnfs archives so the live SMB/NFS cases run
# the title's numeric path and not the Mac's working resolver. See the file's
# header.
add_executable(splat_tests
    test_main.cpp
    resolver_shim.cpp
    ${SPLAT_TEST_SOURCES}
)

target_link_libraries(splat_tests PRIVATE splat_core)
target_include_directories(splat_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

if(SPLAT_HAVE_VULKAN)
    target_compile_definitions(splat_tests PRIVATE SPLAT_SHADER_DIR="${SPLAT_SHADER_DIR}")
    add_dependencies(splat_tests splat_shaders)
endif()

add_test(NAME splat_tests COMMAND splat_tests)
