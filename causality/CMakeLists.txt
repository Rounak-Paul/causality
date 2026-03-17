file(GLOB_RECURSE CAUSALITY_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(causality STATIC ${CAUSALITY_SOURCES})

# shaderc — bundled with the LunarG Vulkan SDK, used for runtime GLSL compilation
find_library(SHADERC_LIB
    NAMES shaderc_combined
    HINTS
        "$ENV{VULKAN_SDK}/lib"
        "$ENV{HOME}/VulkanSDK/1.4.328.1/macOS/lib"
        "/usr/local/lib"
    REQUIRED)
message(STATUS "shaderc_combined: ${SHADERC_LIB}")

# shaderc headers (shaderc/shaderc.h)
find_path(SHADERC_INCLUDE
    NAMES shaderc/shaderc.h
    HINTS
        "$ENV{VULKAN_SDK}/include"
        "$ENV{HOME}/VulkanSDK/1.4.328.1/macOS/include"
        "/usr/local/include"
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
        ${CMAKE_SOURCE_DIR}/vendors/stb
)

target_link_libraries(causality
    PUBLIC
        Vulkan::Vulkan
        glfw
        VulkanMemoryAllocator
        glm-header-only
        Threads::Threads
        ${SHADERC_LIB}
        $<$<PLATFORM_ID:Darwin>:c++>
        $<$<PLATFORM_ID:Linux>:stdc++>
)

target_precompile_headers(causality PRIVATE src/pch.h)
