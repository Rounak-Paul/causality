file(GLOB_RECURSE CAUSALITY_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(causality SHARED ${CAUSALITY_SOURCES})

# Set default symbol visibility to hidden on GCC/Clang so that only
# CA_API-decorated symbols are exported from the shared library.
if(NOT WIN32)
    target_compile_options(causality PRIVATE -fvisibility=hidden)
endif()

# shaderc — bundled with the LunarG Vulkan SDK, used for runtime GLSL compilation
# Derive the SDK root from find_package(Vulkan) or the VULKAN_SDK env var
set(_VULKAN_SDK_HINT "")
if(DEFINED ENV{VULKAN_SDK})
    set(_VULKAN_SDK_HINT "$ENV{VULKAN_SDK}")
elseif(Vulkan_INCLUDE_DIR)
    get_filename_component(_VULKAN_SDK_HINT "${Vulkan_INCLUDE_DIR}" DIRECTORY)
endif()

find_library(SHADERC_LIB
    NAMES shaderc_combined
    HINTS
        "${_VULKAN_SDK_HINT}/lib"
        "${_VULKAN_SDK_HINT}/Lib"
    PATHS
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/aarch64-linux-gnu
    REQUIRED)
message(STATUS "shaderc_combined: ${SHADERC_LIB}")

find_path(SHADERC_INCLUDE
    NAMES shaderc/shaderc.h
    HINTS
        "${_VULKAN_SDK_HINT}/include"
        "${_VULKAN_SDK_HINT}/Include"
    PATHS
        /usr/local/include
        /usr/include
    REQUIRED)

target_include_directories(causality
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${SHADERC_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../vendors/stb
)

target_link_libraries(causality
    PUBLIC
        Vulkan::Vulkan     # Vk* types appear in the public causality.h API
    PRIVATE
        glfw               # internal windowing — not in public headers
        VulkanMemoryAllocator  # internal GPU memory — not in public headers
        glm-header-only    # internal math — not in public headers
        Threads::Threads   # internal threading — not in public headers
        ${SHADERC_LIB}     # internal shader compilation — not in public headers
)

# Platform-specific link libraries
if(APPLE)
    # VMA is C++ — need the C++ stdlib on Apple
    target_link_libraries(causality PUBLIC c++)
elseif(WIN32)
    # Nothing extra needed — MSVC links CRT automatically
else()
    # Linux: VMA needs C++ stdlib; dl/m for dlopen/math
    target_link_libraries(causality PUBLIC stdc++ dl m)
endif()

# Skip precompiled header for the large embedded font data files
set_source_files_properties(src/renderer/embedded_font.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(src/renderer/embedded_font_bold.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_precompile_headers(causality PRIVATE src/pch.h)
