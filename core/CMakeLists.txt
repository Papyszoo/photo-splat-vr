# Core library. Every .cpp under core/<component>/ is compiled, so a new
# component only adds files, not CMake edits. gpu_probe.cpp needs Vulkan and is
# filtered out on builds without it.
file(GLOB SPLAT_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*/*.cpp")

if(NOT SPLAT_HAVE_VULKAN)
    list(REMOVE_ITEM SPLAT_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/render/gpu_probe.cpp")
endif()

add_library(splat_core STATIC ${SPLAT_CORE_SOURCES})

# The panel font (core/ui/font.cpp) is compiled in, so the title needs no font
# file and no system font: third_party/inter, SIL OFL 1.1.
include("${PROJECT_SOURCE_DIR}/cmake/embed_file.cmake")
splat_embed_file(splat_core "${PROJECT_SOURCE_DIR}/third_party/inter/Inter-Medium.ttf" kUiFontData)

# miniz has no CMake project in third_party/ and its headers include a
# CMake-generated miniz_export.h, which the vendored copy does not ship; the
# shim lives in core/common. zip_archive.cpp (the checkpoint reader) needs it,
# so this target belongs to core and must be defined for every build, with or
# without Vulkan.
add_library(splat_miniz STATIC
    "${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c"
    "${PROJECT_SOURCE_DIR}/third_party/miniz/miniz_tdef.c"
    "${PROJECT_SOURCE_DIR}/third_party/miniz/miniz_tinfl.c"
    "${PROJECT_SOURCE_DIR}/third_party/miniz/miniz_zip.c"
)
target_include_directories(splat_miniz PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${PROJECT_SOURCE_DIR}/third_party/miniz"
)

target_include_directories(splat_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/third_party"
    "${PROJECT_SOURCE_DIR}/third_party/json")
target_link_libraries(splat_core PUBLIC ggml splat_miniz)
target_compile_definitions(splat_core PUBLIC SPLAT_VERSION="${PROJECT_VERSION}")

# Shaders -> SPIR-V. Every .comp/.vert/.frag under render/shaders is compiled
# into ${CMAKE_BINARY_DIR}/shaders/<name>.spv.
set(SPLAT_SHADER_DIR "${CMAKE_BINARY_DIR}/shaders" CACHE PATH "Compiled SPIR-V directory")
set(SPLAT_SHADER_SOURCES "")
foreach(ext comp vert frag)
    file(GLOB _splat_shaders CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/*.${ext}")
    list(APPEND SPLAT_SHADER_SOURCES ${_splat_shaders})
endforeach()

# Shaders are only needed by the Vulkan renderer, so a CPU-only build does not
# require glslc.
if(SPLAT_SHADER_SOURCES AND SPLAT_HAVE_VULKAN)
    if(NOT Vulkan_GLSLC_EXECUTABLE)
        find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc HINTS "$ENV{VULKAN_SDK}/bin")
    endif()
    if(NOT Vulkan_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc not found; install shaderc (brew install shaderc) or the Vulkan SDK")
    endif()

    set(SPLAT_SPIRV_OUTPUTS "")
    foreach(shader IN LISTS SPLAT_SHADER_SOURCES)
        get_filename_component(shader_name "${shader}" NAME)
        set(spirv "${SPLAT_SHADER_DIR}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${spirv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SPLAT_SHADER_DIR}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O
                    -o "${spirv}" "${shader}"
            DEPENDS "${shader}"
            COMMENT "glslc ${shader_name}"
            VERBATIM)
        list(APPEND SPLAT_SPIRV_OUTPUTS "${spirv}")
    endforeach()

    add_custom_target(splat_shaders ALL DEPENDS ${SPLAT_SPIRV_OUTPUTS})
endif()

if(SPLAT_HAVE_VULKAN)
    target_link_libraries(splat_core PUBLIC Vulkan::Vulkan)
    # recon/ggml/backend.cpp initializes the ggml Vulkan backend directly
    # (ggml_backend_vk_init) instead of relying on the dynamic loader.
    target_link_libraries(splat_core PUBLIC ggml-vulkan)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_VULKAN=1)
endif()
