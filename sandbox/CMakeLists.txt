file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(sandbox ${SANDBOX_SOURCES})

target_link_libraries(sandbox PRIVATE causality)

# Pass the absolute path to the font so the sandbox always finds it
target_compile_definitions(sandbox PRIVATE
    CA_FONT_PATH="${CMAKE_SOURCE_DIR}/assets/fonts/DepartureMonoNerdFont-Regular.otf")
