# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

laghu_manifest_sources(standalone LAGHU_STANDALONE_SOURCES)
add_library(laghu_proxy STATIC ${LAGHU_STANDALONE_SOURCES})
target_include_directories(laghu_proxy PUBLIC include)
target_compile_features(laghu_proxy PUBLIC c_std_11)
target_link_libraries(laghu_proxy PUBLIC Laghu::Http Laghu::Yaml Threads::Threads
                                          OpenSSL::SSL OpenSSL::Crypto)

add_executable(laghu src/main.c)
target_link_libraries(laghu PRIVATE laghu_proxy)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_proxy PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(laghu PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(laghu_proxy_test tests/proxy_test.c)
  target_include_directories(laghu_proxy_test PRIVATE src)
  target_link_libraries(laghu_proxy_test PRIVATE laghu_proxy)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_proxy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_proxy_test COMMAND laghu_proxy_test)
  add_test(NAME laghu_proxy_smoke
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/proxy_smoke.py
                   $<TARGET_FILE:laghu>
                   $<TARGET_FILE:laghu_runtime_cache_fixture>
                   ${LAGHU_JS_BINARY})
  if(LAGHU_HAVE_VIPS)
    add_test(NAME laghu_passthrough_queue_reload_test
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/passthrough_queue_reload_test.py
                     $<TARGET_FILE:laghu>
                     $<TARGET_FILE:laghu-libvips>)
  endif()
  add_test(NAME laghu_parity_contract_test
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity_contract_test.py
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/parity-contract.json)
  add_test(NAME laghu_execution_rail_test
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/execution_rail_test.py)
endif()
