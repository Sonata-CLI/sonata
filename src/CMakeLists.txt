include(GNUInstallDirs)

configure_file(${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/sonata/version.hpp @ONLY)

add_library(sonata
  # systems
  core/package.cpp
  core/project.cpp
  core/module.cpp
  core/library.cpp
  # luau systems
  core/luau/compiler.cpp
  core/luau/datafile.cpp
  core/luau/environment.cpp
  core/luau/vm.cpp
  # builtin libs
  #core/libs/example.cpp
  core/libs/net.cpp
  core/libs/fs.cpp
  core/libs/path.cpp
  core/libs/sys.cpp
  core/libs/process.cpp
  core/libs/task.cpp
  core/libs/stdio.cpp
  core/libs/json.cpp
  core/libs/crypto.cpp
  core/libs/datetime.cpp
)
add_library(sonata::sonata ALIAS sonata)

target_compile_features(sonata PUBLIC cxx_std_20)

target_include_directories(sonata
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

find_package(OpenSSL REQUIRED)
target_link_libraries(sonata PUBLIC Luau.Common Luau.VM Luau.Compiler Luau.Ast OpenSSL::SSL OpenSSL::Crypto)

# ---- Build metadata for sonata/core/info.hpp ----
# Version numbers come from project(... VERSION x.y.z) in the root CMakeLists.
# The root may also pre-set SONATA_VERSION_PRE, SONATA_GIT_HASH and
# SONATA_LUAU_VERSION; anything it doesn't set is filled in or left empty here.
if(NOT DEFINED SONATA_GIT_HASH)
  set(SONATA_GIT_HASH "")
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE SONATA_GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE git_result
      ERROR_QUIET)
    if(NOT git_result EQUAL 0)
      set(SONATA_GIT_HASH "")
    endif()
  endif()
endif()

set(sonata_info_defs
  "SONATA_NET_TLS"
  "SONATA_GIT_HASH=\"${SONATA_GIT_HASH}\""
  "SONATA_LUAU_VERSION=\"${SONATA_LUAU_VERSION}\""
  "SONATA_VERSION_PRE=\"${SONATA_VERSION_PRE}\""
  "SONATA_BUILD_TYPE=\"$<LOWER_CASE:$<CONFIG>>\"")

foreach(part MAJOR MINOR PATCH)
  if(NOT "${PROJECT_VERSION_${part}}" STREQUAL "")
    list(APPEND sonata_info_defs SONATA_VERSION_${part}=${PROJECT_VERSION_${part}})
  endif()
endforeach()

# PUBLIC so the CLI and installed consumers see the same values as the
# library (info.hpp is all inline constexpr).
target_compile_definitions(sonata PUBLIC ${sonata_info_defs})

if(SONATA_INSTALL AND SONATA_OWNS_LUAU)
  include(CMakePackageConfigHelpers)

  set(SONATA_LUAU_TARGETS Luau.Common Luau.Ast Luau.Compiler Luau.VM Luau.Bytecode)
  set(SONATA_LUAU_INCLUDE_DEST ${CMAKE_INSTALL_INCLUDEDIR}/sonata/luau)

  # Make Luau's include paths exportable
  foreach(t IN LISTS SONATA_LUAU_TARGETS)
    set(fixed "")
    get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
      foreach(d IN LISTS dirs)
        list(APPEND fixed "$<BUILD_INTERFACE:${d}>")
      endforeach()
    endif()
    list(APPEND fixed "$<INSTALL_INTERFACE:${SONATA_LUAU_INCLUDE_DEST}>")
    set_target_properties(${t} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${fixed}")
  endforeach()

  # Install Luau's public headers somewhere private to sonata
  foreach(sub Common Ast Compiler VM Bytecode)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/external/luau/${sub}/include/
            DESTINATION ${SONATA_LUAU_INCLUDE_DEST})
  endforeach()

  install(TARGETS sonata ${SONATA_LUAU_TARGETS} EXPORT sonataTargets)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT sonataTargets NAMESPACE sonata:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)

  configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/sonataConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sonataConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/sonataConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sonataConfig.cmake
                ${CMAKE_CURRENT_BINARY_DIR}/sonataConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sonata)
endif()
