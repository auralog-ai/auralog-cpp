cmake_minimum_required(VERSION 3.20)
project(auralog-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AURALOG_BUILD_TESTS "Build Auralog C++ tests" ON)
option(AURALOG_BUILD_EXAMPLES "Build Auralog C++ examples" ON)
option(AURALOG_ENABLE_SANITIZERS "Build with address and undefined behavior sanitizers" OFF)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL REQUIRED)

add_library(auralog
  src/auralog.cpp
  src/curl_transport.cpp
  src/terminate_capture.cpp
)
add_library(auralog::auralog ALIAS auralog)

target_compile_features(auralog PUBLIC cxx_std_17)
target_include_directories(auralog
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(auralog PUBLIC nlohmann_json::nlohmann_json CURL::libcurl)

if(AURALOG_ENABLE_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "AURALOG_ENABLE_SANITIZERS is not supported with MSVC")
  endif()
  target_compile_options(auralog PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(auralog PUBLIC -fsanitize=address,undefined)
endif()

if(AURALOG_BUILD_EXAMPLES)
  add_executable(auralog_basic examples/basic.cpp)
  target_link_libraries(auralog_basic PRIVATE auralog::auralog)
endif()

if(AURALOG_BUILD_TESTS)
  enable_testing()
  add_executable(auralog_sdk_test tests/sdk_test.cpp)
  target_link_libraries(auralog_sdk_test PRIVATE auralog::auralog)
  add_test(NAME auralog_sdk_test COMMAND auralog_sdk_test)
endif()

include(GNUInstallDirs)
install(TARGETS auralog
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
