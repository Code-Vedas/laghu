# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

laghu_manifest_sources(markup LAGHU_MARKUP_SOURCES)
add_library(laghu_markup STATIC ${LAGHU_MARKUP_SOURCES})
add_library(Laghu::Markup ALIAS laghu_markup)
target_include_directories(laghu_markup PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_markup PUBLIC c_std_11)
target_link_libraries(laghu_markup PUBLIC Laghu::Base)
set_target_properties(laghu_markup PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_markup PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_markup_test tests/markup_test.c)
  target_link_libraries(laghu_markup_test PRIVATE Laghu::Markup)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_markup_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_markup_test COMMAND laghu_markup_test)
endif()
