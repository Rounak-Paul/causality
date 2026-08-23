cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(causality LANGUAGES C CXX OBJC)
else()
    project(causality LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

# MSVC runtime library — always the Release DLL CRT, even in Debug config.
# The LunarG Vulkan SDK ships shaderc_combined.lib (linked below) built only
# against the Release CRT, with no debug variant; a Debug-CRT (/MDd) build
# fails to link with LNK2038 CRT mismatches. Debug configs still compile at
# -Od with full debug symbols — only /MDd's extra iterator/heap-debug checks
# are unavailable. Mirrors the same override in the top-level QuasarEngine
# CMakeLists.txt, needed here too since causality is also buildable standalone.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    # Suppress common MSVC warnings for POSIX-like C code
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Output directories (only when built standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Vulkan
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# Platform definitions
if(APPLE)
    add_compile_definitions(CA_PLATFORM_APPLE)
elseif(WIN32)
    add_compile_definitions(CA_PLATFORM_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX)
    add_compile_definitions(CA_PLATFORM_LINUX)
endif()

# Vendors
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(vendors/glfw)

add_subdirectory(vendors/vma)

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vendors/glm)

# FreeType — high-quality glyph rasterization with LCD subpixel filter and autohinting.
# Disable all optional codec deps to keep the vendor build self-contained.
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
set(FT_ENABLE_ERROR_STRINGS OFF CACHE BOOL "" FORCE)
# freetype is statically linked into libcausality — must be PIC on Linux/ELF
if(UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
add_subdirectory(vendors/freetype)

# Library
add_subdirectory(causality)

# Sandbox (only when causality is the top-level project)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(sandbox)
endif()
