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

# --- zstd, decompression only (L2-A) ---------------------------------------
# spz v4 stores one zstd frame per attribute stream. The submodule is vendored
# whole but only its decompressor is compiled (third_party/zstd_sources.cmake),
# so the payload never carries a compressor it cannot use. A tree without the
# submodule still builds and reports that spz v4 is not supported.
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/zstd/lib/zstd.h")
    set(SPLAT_ZSTD_DIR "${PROJECT_SOURCE_DIR}/third_party/zstd")
    include("${PROJECT_SOURCE_DIR}/third_party/zstd_sources.cmake")
    add_library(splat_zstd STATIC ${SPLAT_ZSTD_SOURCES})
    target_include_directories(splat_zstd PUBLIC "${SPLAT_ZSTD_DIR}/lib")
    target_compile_definitions(splat_zstd PUBLIC
        ZSTD_LEGACY_SUPPORT=0
        ZSTD_MULTITHREAD=0
        ZSTD_DISABLE_ASM=1
        # The tracing hooks are weak undefined symbols; a PS5 title may export
        # nothing, and the console link refuses them.
        ZSTD_TRACE=0)
    target_link_libraries(splat_core PUBLIC splat_zstd)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_ZSTD=1)
endif()

# --- image codecs (L1-B1) --------------------------------------------------
# WebP and TIFF are configured by the top-level CMakeLists.txt; link them only
# when the subprojects really produced their targets, so a tree in which one of
# them did not configure still builds (the decoders then report "support is not
# built into this version" and can_test skips those cases).
if(TARGET webp)
    target_link_libraries(splat_core PUBLIC webp)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_WEBP=1)
endif()
if(TARGET tiff)
    target_link_libraries(splat_core PUBLIC tiff)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_TIFF=1)
endif()
# HEIF/AVIF: the three archives from the codec prefix (see the top-level
# CMakeLists.txt), linked by path so the imported config's absolute paths and
# its AOM::aom dependency never leak into the title link.
if(SPLAT_HAVE_HEIF)
    target_include_directories(splat_core PUBLIC "${LIBHEIF_INCLUDE_DIR}")
    target_link_libraries(splat_core PUBLIC "${LIBHEIF_LIBRARY}" "${LIBDE265_LIBRARY}"
                                          "${LIBAOM_LIBRARY}")
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_HEIF=1)
endif()

# --- SMB through libsmb2 (L3-B) --------------------------------------------
# Linked when the subproject configured; a tree without the submodule still
# builds and core/vfs/location.cpp reports every SMB location offline. The
# adapters are compiled only when the header is there as well (core/CMakeLists
# globs every .cpp, so the guard is the compile definition the sources use).
if(TARGET smb2)
    target_link_libraries(splat_core PUBLIC smb2)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_SMB2=1)
endif()

# --- NFS through libnfs (L4) -----------------------------------------------
# Linked when the subproject configured; a tree without the submodule still
# builds and core/vfs/nfs.cpp reports every NFS location offline.
if(TARGET nfs)
    target_link_libraries(splat_core PUBLIC nfs)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_NFS=1)
endif()

# --- RAW through LibRaw (L1-C) ---------------------------------------------
# LibRaw has no CMake project: third_party/libraw_sources.cmake lists its
# sources once, and this target compiles them with -DLIBRAW_NOTHREADS and
# without USE_ZLIB/USE_JPEG/USE_LCMS (the payload SDK has none of them, and the
# RAW path never writes a file). A tree without the submodule still builds and
# reports that RAW support is not in.
if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libraw/buildfiles/libraw.pro")
    set(SPLAT_LIBRAW_DIR "${PROJECT_SOURCE_DIR}/third_party/libraw")
    include("${PROJECT_SOURCE_DIR}/third_party/libraw_sources.cmake")
    add_library(splat_libraw STATIC ${SPLAT_LIBRAW_SOURCES})
    # LibRaw's headers are included as <libraw/libraw.h>, so the tree root is
    # the include directory.
    target_include_directories(splat_libraw PUBLIC "${SPLAT_LIBRAW_DIR}")
    target_compile_definitions(splat_libraw PUBLIC LIBRAW_NOTHREADS)
    target_link_libraries(splat_core PUBLIC splat_libraw)
    target_compile_definitions(splat_core PUBLIC SPLAT_HAVE_RAW=1)
endif()

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
