# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED LAGHU_SOURCE)
  message(FATAL_ERROR "Laghu sanitizer reconfigure expectation requires LAGHU_SOURCE")
endif()

set(CMAKE_SOURCE_DIR "${LAGHU_SOURCE}")
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_CXX_COMPILER_ID Clang)
include("${LAGHU_SOURCE}/cmake/LaghuSanitizers.cmake")

set(LAGHU_SANITIZER_PROFILE ASAN_UBSAN)
laghu_configure_sanitizer_profile()
if(NOT LAGHU_SANITIZER_COMPILE_OPTIONS MATCHES "-fsanitize=address,undefined" OR
    NOT LAGHU_SANITIZER_COMPILE_OPTIONS MATCHES "-fno-sanitize-recover=undefined" OR
    NOT LAGHU_SANITIZER_LINK_OPTIONS STREQUAL "-fsanitize=address,undefined")
  message(FATAL_ERROR "Laghu sanitizer reconfigure expectation failed: asan_ubsan_flags_missing")
endif()

set(LAGHU_SANITIZER_PROFILE NONE)
laghu_configure_sanitizer_profile()
if(NOT LAGHU_SANITIZER_COMPILE_OPTIONS STREQUAL "" OR
    NOT LAGHU_SANITIZER_LINK_OPTIONS STREQUAL "")
  message(FATAL_ERROR "Laghu sanitizer reconfigure expectation failed: none_retained_stale_flags")
endif()

set(LAGHU_SANITIZER_PROFILE TSAN)
laghu_configure_sanitizer_profile()
if(NOT LAGHU_SANITIZER_COMPILE_OPTIONS MATCHES "-fsanitize=thread" OR
    LAGHU_SANITIZER_COMPILE_OPTIONS MATCHES "address" OR
    NOT LAGHU_SANITIZER_LINK_OPTIONS STREQUAL "-fsanitize=thread")
  message(FATAL_ERROR "Laghu sanitizer reconfigure expectation failed: tsan_retained_stale_flags")
endif()
