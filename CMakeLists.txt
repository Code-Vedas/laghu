# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.20)

project(laghu VERSION 0.1.0 LANGUAGES C)

include(CTest)

option(LAGHU_BUILD_TESTS "Build the Laghu core test suite" ${BUILD_TESTING})
option(LAGHU_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(LAGHU_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)
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

add_subdirectory(libs/laghu-core)
add_subdirectory(libs/laghu-image)
if(LAGHU_BUILD_SERVICE)
  add_subdirectory(libs/laghu-runtime)
  add_subdirectory(libs/laghu-http)
  add_subdirectory(workers/laghu-libvips)
  if(LAGHU_BUILD_PROXY)
    add_subdirectory(servers/laghu)
  endif()
endif()
