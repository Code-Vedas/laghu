# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_executable(laghu-html-refresh main.c)
target_link_libraries(laghu-html-refresh PRIVATE Laghu::Runtime OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(laghu-html-refresh PRIVATE c_std_11)
target_compile_definitions(
  laghu-html-refresh PRIVATE LAGHU_TEST_HOOKS=$<BOOL:${LAGHU_BUILD_TESTS}>
)

if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(laghu_html_refresh_fixture tests/refresh_fixture.c)
  target_link_libraries(laghu_html_refresh_fixture PRIVATE Laghu::Runtime)
  target_compile_features(laghu_html_refresh_fixture PRIVATE c_std_11)
  add_test(NAME laghu_html_refresh_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/refresh_test.py
                   $<TARGET_FILE:laghu-html-refresh>
                   $<TARGET_FILE:laghu_html_refresh_fixture>)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-html-refresh PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(LAGHU_BUILD_TESTS)
    target_compile_options(laghu_html_refresh_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
