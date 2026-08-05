# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

laghu_manifest_sources(core LAGHU_CORE_SOURCES)
add_library(laghu_core STATIC ${LAGHU_CORE_SOURCES})

add_library(Laghu::Core ALIAS laghu_core)

target_include_directories(laghu_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(laghu_core PUBLIC c_std_11)
target_link_libraries(laghu_core PUBLIC Laghu::Base)
set_target_properties(laghu_core PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(laghu_core PRIVATE /W4 /WX)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_core_test tests/core_test.c)
  target_link_libraries(laghu_core_test PRIVATE Laghu::Core)
  add_executable(laghu_config_test tests/config_test.c)
  target_link_libraries(laghu_config_test PRIVATE Laghu::Core)

  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(laghu_config_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(laghu_core_test PRIVATE /W4 /WX)
    target_compile_options(laghu_config_test PRIVATE /W4 /WX)
  endif()

  add_test(NAME laghu_core_test COMMAND laghu_core_test)
  add_test(NAME laghu_config_test COMMAND laghu_config_test)
endif()

if(LAGHU_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LAGHU_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(laghu_core_policy_fuzz tests/policy_fuzz.c)
  target_link_libraries(laghu_core_policy_fuzz PRIVATE Laghu::Core)
  target_compile_options(laghu_core_policy_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(laghu_core_policy_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
