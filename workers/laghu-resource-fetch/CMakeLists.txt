# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_executable(laghu-resource-fetch main.c)
target_link_libraries(laghu-resource-fetch PRIVATE Laghu::Runtime OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(laghu-resource-fetch PRIVATE c_std_11)
target_compile_definitions(
  laghu-resource-fetch PRIVATE LAGHU_TEST_HOOKS=$<BOOL:${LAGHU_BUILD_TESTS}>
)

if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME laghu_resource_fetch_test
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/fetch_test.py
                   $<TARGET_FILE:laghu-resource-fetch>
                   $<TARGET_FILE:laghu_runtime_cache_fixture>)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-resource-fetch PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
