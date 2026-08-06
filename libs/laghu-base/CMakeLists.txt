# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

laghu_manifest_sources(base LAGHU_BASE_SOURCES)
add_library(laghu_base STATIC ${LAGHU_BASE_SOURCES})
add_library(Laghu::Base ALIAS laghu_base)

target_include_directories(laghu_base PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_base PUBLIC c_std_11)
set_target_properties(laghu_base PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_base PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_base_test tests/base_test.c)
  target_link_libraries(laghu_base_test PRIVATE Laghu::Base)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_base_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_base_test COMMAND laghu_base_test)
endif()
