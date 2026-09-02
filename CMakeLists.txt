# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.20)

project(laghu LANGUAGES C)

include(CTest)
include(cmake/LaghuSources.cmake)

option(LAGHU_BUILD_TESTS "Build the Laghu core test suite" ${BUILD_TESTING})
option(LAGHU_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(LAGHU_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)
option(
  LAGHU_ENABLE_RELEASE_HARDENING
  "Require Linux Release C hardening for production artifacts"
  ON
)
option(
  LAGHU_BUILD_SERVICE
  "Build the queue/cache runtime and laghu-libvips service"
  ON
)
option(LAGHU_BUILD_PROXY "Build the standalone Laghu HTTP reverse proxy" ON)
set(LAGHU_WITH_VIPS "AUTO" CACHE STRING "Build libvips image support: AUTO, ON, or OFF")
set_property(CACHE LAGHU_WITH_VIPS PROPERTY STRINGS AUTO ON OFF)

if(LAGHU_ENABLE_SANITIZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "LAGHU_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# This is deliberately Linux/ELF scoped. Packaging and container release builds
# select CMAKE_BUILD_TYPE=Release, so these flags cover every CMake C target
# created below without making claims about non-ELF toolchains.
if(LAGHU_ENABLE_RELEASE_HARDENING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Linux release hardening requires a GNU or Clang C compiler")
  endif()

  cmake_policy(SET CMP0083 NEW)
  include(CheckPIESupported)
  check_pie_supported(OUTPUT_VARIABLE LAGHU_PIE_ERROR LANGUAGES C)
  if(NOT CMAKE_C_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR "Linux release hardening requires PIE linker support: ${LAGHU_PIE_ERROR}")
  endif()

  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  string(APPEND CMAKE_C_FLAGS_RELEASE " -fstack-protector-strong -D_FORTIFY_SOURCE=3")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " -Wl,-z,relro,-z,now")
  string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE " -Wl,-z,relro,-z,now")
  string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE " -Wl,-z,relro,-z,now")
endif()

add_subdirectory(third_party/libyaml)
add_subdirectory(libs/laghu-base)
add_subdirectory(libs/laghu-markup)
add_subdirectory(libs/laghu-core)
add_subdirectory(libs/laghu-image)
if(LAGHU_BUILD_SERVICE)
  add_subdirectory(libs/laghu-runtime)
  add_subdirectory(libs/laghu-http)
  add_subdirectory(workers/laghu-libvips)
  add_subdirectory(workers/laghu-resource-fetch)
  add_subdirectory(workers/laghu-html-refresh)
  add_subdirectory(workers/laghu-asset-upload)
  add_subdirectory(workers/laghu-js-optimize)
  add_subdirectory(workers/laghu-chrome-analyze)
  add_subdirectory(workers/laghu-otel-export)
  if(LAGHU_BUILD_PROXY)
    add_subdirectory(servers/laghu)
  endif()
endif()
