# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.20)
project(mod_laghu_windows LANGUAGES C)

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED APACHE_ROOT)
  message(FATAL_ERROR "LAGHU_SOURCE and APACHE_ROOT are required")
endif()

find_library(HTTPD_LIBRARY NAMES libhttpd PATHS "${APACHE_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(APR_LIBRARY NAMES libapr-1 PATHS "${APACHE_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(APRUTIL_LIBRARY NAMES libaprutil-1 PATHS "${APACHE_ROOT}/lib" REQUIRED NO_DEFAULT_PATH)

file(STRINGS "${LAGHU_SOURCE}/cmake/laghu-sources.list" LAGHU_MANIFEST)
set(LAGHU_SHARED_SOURCES)
foreach(entry IN LISTS LAGHU_MANIFEST)
  if(entry MATCHES "^(base|markup|core|image|runtime|http|apache)\\|[^|]+\\|(.+)$")
    list(APPEND LAGHU_SHARED_SOURCES "${LAGHU_SOURCE}/${CMAKE_MATCH_2}")
  endif()
endforeach()
add_library(mod_laghu MODULE
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/runtime_win32.c"
  ${LAGHU_SHARED_SOURCES}
)
target_compile_features(mod_laghu PRIVATE c_std_11)
target_compile_definitions(mod_laghu PRIVATE LAGHU_HAVE_VIPS=0)
target_include_directories(mod_laghu PRIVATE
  "${APACHE_ROOT}/include"
  "${LAGHU_SOURCE}/libs/laghu-http/include"
  "${LAGHU_SOURCE}/libs/laghu-http/src"
  "${LAGHU_SOURCE}/libs/laghu-base/include"
  "${LAGHU_SOURCE}/libs/laghu-markup/include"
  "${LAGHU_SOURCE}/libs/laghu-core/include"
  "${LAGHU_SOURCE}/libs/laghu-image/include"
  "${LAGHU_SOURCE}/libs/laghu-runtime/include"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src"
  "${LAGHU_SOURCE}/modules/mod_laghu"
)
target_link_libraries(mod_laghu PRIVATE
  "${HTTPD_LIBRARY}" "${APR_LIBRARY}" "${APRUTIL_LIBRARY}" ws2_32
)
set_target_properties(mod_laghu PROPERTIES PREFIX "" SUFFIX ".so")
