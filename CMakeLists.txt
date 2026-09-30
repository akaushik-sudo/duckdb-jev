cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME snx_jev)

find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
  src/include
  third_party/httplib
  third_party/json)

set(EXTENSION_SOURCES
  src/snx_jev_extension.cpp
  src/jev_functions.cpp
  src/jev_client.cpp
  src/jev_config.cpp
  src/jev_json.cpp
  src/jev_state.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

find_package(Threads REQUIRED)

foreach(TARGET ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_link_libraries(${TARGET} OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
  if(APPLE)
    # OpenSSL on macOS has no usable default CA store; cpp-httplib can read the
    # system roots out of the keychain instead.
    target_compile_definitions(${TARGET} PRIVATE CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
    target_link_libraries(${TARGET} "-framework CoreFoundation" "-framework Security")
  endif()
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
