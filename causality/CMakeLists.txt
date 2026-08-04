file(GLOB_RECURSE CAUSALITY_SOURCES CONFIGURE_DEPENDS src/*.c)

if(APPLE)
    list(APPEND CAUSALITY_SOURCES src/platform/mouse_state_mac.m)
endif()

add_library(causality STATIC ${CAUSALITY_SOURCES})

# Set default symbol visibility to hidden on GCC/Clang so that only
# CA_API-decorated symbols are exported from the final binary.
# causality_EXPORTS makes CA_API expand to visibility("default"), which
# overrides -fvisibility=hidden for the public API surface.
if(NOT WIN32)
    target_compile_options(causality PRIVATE -fvisibility=hidden)
    target_compile_definitions(causality PRIVATE causality_EXPORTS)
endif()

# Enable the Vulkan validation layer + debug messenger in Debug and
# RelWithDebInfo builds only — compiled out entirely in Release, matching
# QS_PROFILE_ENABLED's convention in Quasar/CMakeLists.txt.
target_compile_definitions(causality PRIVATE
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:CAUSALITY_VULKAN_VALIDATION>
)

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
    PRIVATE
        Vulkan::Vulkan     # Vk* types only appear in the opt-in ca_gpu.h header;
                           # consumers of that header provide Vulkan themselves
        glfw               # internal windowing — not in public headers
        VulkanMemoryAllocator  # internal GPU memory — not in public headers
        glm-header-only    # internal math — not in public headers
        Threads::Threads   # internal threading — not in public headers
        ${SHADERC_LIB}     # internal shader compilation — not in public headers
        freetype           # internal glyph rasterization — not in public headers
)

# Platform-specific link libraries
if(APPLE)
    # C-only consumers need VMA/shaderc's C++ runtime explicitly. C++ link
    # drivers already add it, so propagating another -lc++ only produces a
    # duplicate-library warning.
    target_link_libraries(causality PUBLIC $<$<LINK_LANGUAGE:C>:c++>)
    target_link_libraries(causality PRIVATE "-framework AppKit")
elseif(WIN32)
    # Nothing extra needed — MSVC links CRT automatically
else()
    # Linux: VMA needs C++ stdlib; dl/m for dlopen/math
    find_package(X11 REQUIRED)
    target_link_libraries(causality PUBLIC stdc++ dl m)
    target_link_libraries(causality PRIVATE X11::X11)
endif()

# Skip precompiled header for the large embedded font data files
set_source_files_properties(src/renderer/embedded_font.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(src/renderer/embedded_font_bold.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_source_files_properties(src/renderer/embedded_symbols_font.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_precompile_headers(causality PRIVATE src/pch.h)

if(BUILD_TESTING)
    add_executable(causality_array_tests
        tests/ca_array_tests.c
        src/core/ca_alloc.c
        src/core/ca_array.c
        src/core/ca_pool.c
    )
    target_include_directories(causality_array_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    add_test(NAME causality_array_tests COMMAND causality_array_tests)

    add_executable(causality_storage_tests tests/ca_storage_tests.c)
    target_include_directories(causality_storage_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    )
    target_link_libraries(causality_storage_tests PRIVATE causality)
    add_test(NAME causality_storage_tests COMMAND causality_storage_tests)

    add_executable(causality_input_capture_tests tests/ca_input_capture_tests.c)
    target_include_directories(causality_input_capture_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    )
    target_link_libraries(causality_input_capture_tests PRIVATE
        causality
        Vulkan::Vulkan
    )
    add_test(NAME causality_input_capture_tests COMMAND causality_input_capture_tests)
endif()
