file(GLOB_RECURSE CAUSALITY_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(causality STATIC ${CAUSALITY_SOURCES})

target_include_directories(causality
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(causality
    PUBLIC
        Vulkan::Vulkan
        glfw
        VulkanMemoryAllocator
        glm-header-only
        Threads::Threads
)

target_precompile_headers(causality PRIVATE src/pch.h)
