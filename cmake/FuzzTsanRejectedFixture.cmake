# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE)
  message(FATAL_ERROR "Laghu fuzz TSAN rejection fixture requires MODULE")
endif()

set(CMAKE_CROSSCOMPILING FALSE)
set(CMAKE_CXX_COMPILER_ID Clang)
include("${MODULE}")
set(LAGHU_BUILD_FUZZERS ON CACHE BOOL "Build Laghu Clang libFuzzer targets" FORCE)
set(LAGHU_SANITIZER_PROFILE TSAN)
laghu_configure_fuzzing()
