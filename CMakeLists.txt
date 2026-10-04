cmake_minimum_required(VERSION 3.21)
project(sonata VERSION 0.1.6 LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(SONATA_BUILD_CLI "Build the sn executable" ${PROJECT_IS_TOP_LEVEL})
option(SONATA_INSTALL   "Generate install rules"  ${PROJECT_IS_TOP_LEVEL})

set(SONATA_BUILD_TYPE "")

set(SONATA_OWNS_LUAU OFF)
if(NOT TARGET Luau.VM)
  set(LUAU_BUILD_CLI   OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/luau)
  set(SONATA_OWNS_LUAU ON)
endif()

find_package(Git QUIET)
set(SONATA_GIT_HASH "")
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE SONATA_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

add_subdirectory(src)
if(SONATA_BUILD_CLI)
  add_subdirectory(cli)
endif()