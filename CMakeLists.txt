cmake_minimum_required(VERSION 3.20)

# Enable Objective-C on Apple (required by GLFW for native windows)
if(APPLE)
    project(causality LANGUAGES C CXX OBJC)
else()
    project(causality LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC runtime library
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # Suppress common MSVC warnings for POSIX-like C code
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

# Library
add_subdirectory(causality)

# Sandbox (only when causality is the top-level project)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(sandbox)
endif()
