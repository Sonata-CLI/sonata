include(GNUInstallDirs)

configure_file(${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/sonata/version.hpp @ONLY)

add_library(sonata
  core/package.cpp core/project.cpp core/module.cpp
  core/luau/compiler.cpp core/luau/datafile.cpp
  core/luau/environment.cpp core/luau/vm.cpp
  core/library.cpp core/libs/example.cpp)
add_library(sonata::sonata ALIAS sonata)   # same name for in-tree and installed use

target_compile_features(sonata PUBLIC cxx_std_20)

target_include_directories(sonata
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(sonata PUBLIC Luau.Common Luau.VM Luau.Compiler Luau.Ast)

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