# cli/CMakeLists.txt
include(GNUInstallDirs)

add_library(sonata_cli STATIC cli.cpp commands/init.cpp commands/pkg.cpp commands/run.cpp)
target_include_directories(sonata_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sonata_cli PUBLIC sonata::sonata)
target_compile_definitions(sonata_cli PRIVATE BUILD_TYPE="$<CONFIG>")

add_executable(sn main.cpp)
target_link_libraries(sn PRIVATE sonata_cli)

install(TARGETS sn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
  get_filename_component(_sonata_ssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
  file(GLOB _sonata_ssl_dlls
    "${_sonata_ssl_root}/bin/libssl-*.dll"
    "${_sonata_ssl_root}/bin/libcrypto-*.dll")
  if(_sonata_ssl_dlls)
    install(FILES ${_sonata_ssl_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
  else()
    message(WARNING "OpenSSL DLLs not found in ${_sonata_ssl_root}/bin; "
                    "the packaged sn.exe will need OpenSSL on PATH")
  endif()
endif()