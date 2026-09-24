cmake_minimum_required(VERSION 3.20)

project(sonata
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Luau
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/external/luau"
    "${CMAKE_BINARY_DIR}/luau"
)

# Dynamic src compiler
# This isnt recommended. I will delete this and type them out manually in the future
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(sn ${SOURCES})


target_include_directories(sn
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Link Luau
target_link_libraries(sn
    PRIVATE
        Luau.Compiler
        Luau.VM
        Luau.Ast
)