# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
add_executable(laghu-otel-export main.c)
target_link_libraries(laghu-otel-export PRIVATE Laghu::Runtime OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(laghu-otel-export PRIVATE c_std_11)
target_compile_definitions(laghu-otel-export PRIVATE LAGHU_TEST_HOOKS=$<BOOL:${LAGHU_BUILD_TESTS}>)
if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(laghu_otel_export_fixture tests/otel_fixture.c)
  target_link_libraries(laghu_otel_export_fixture PRIVATE Laghu::Runtime)
  target_compile_features(laghu_otel_export_fixture PRIVATE c_std_11)
  add_test(NAME laghu_otel_export_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/otel_test.py $<TARGET_FILE:laghu-otel-export> $<TARGET_FILE:laghu_otel_export_fixture>)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-otel-export PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(LAGHU_BUILD_TESTS)
    target_compile_options(laghu_otel_export_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
