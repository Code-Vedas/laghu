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

add_library(mod_laghu MODULE
  "${LAGHU_SOURCE}/modules/mod_laghu/mod_laghu.c"
  "${LAGHU_SOURCE}/libs/laghu-http/src/http.c"
  "${LAGHU_SOURCE}/libs/laghu-core/src/core.c"
  "${LAGHU_SOURCE}/libs/laghu-image/src/image.c"
  "${LAGHU_SOURCE}/libs/laghu-image/src/markup.c"
  "${LAGHU_SOURCE}/libs/laghu-image/src/css.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/runtime_win32.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/catalog.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/html.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/html_head.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/html_hints.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/css_runtime.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/css_catalog.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/css_combine.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/css_imports.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/css_markup.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/critical_css.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/instrumentation.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/font_provider.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/javascript.c"
  "${LAGHU_SOURCE}/libs/laghu-runtime/src/beacon.c"
)
target_compile_features(mod_laghu PRIVATE c_std_11)
target_compile_definitions(mod_laghu PRIVATE LAGHU_HAVE_VIPS=0)
target_include_directories(mod_laghu PRIVATE
  "${APACHE_ROOT}/include"
  "${LAGHU_SOURCE}/libs/laghu-http/include"
  "${LAGHU_SOURCE}/libs/laghu-core/include"
  "${LAGHU_SOURCE}/libs/laghu-image/include"
  "${LAGHU_SOURCE}/libs/laghu-runtime/include"
)
target_link_libraries(mod_laghu PRIVATE
  "${HTTPD_LIBRARY}" "${APR_LIBRARY}" "${APRUTIL_LIBRARY}"
)
set_target_properties(mod_laghu PROPERTIES PREFIX "" SUFFIX ".so")
