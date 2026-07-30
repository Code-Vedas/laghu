# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_library(laghu_proxy STATIC src/proxy.c)
target_include_directories(laghu_proxy PUBLIC include)
target_compile_features(laghu_proxy PUBLIC c_std_11)
target_link_libraries(laghu_proxy PUBLIC Laghu::Http Threads::Threads OpenSSL::SSL
                                          OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(laghu_proxy PUBLIC ws2_32)
endif()

add_executable(laghu src/main.c)
target_link_libraries(laghu PRIVATE laghu_proxy)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_proxy PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(laghu PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(laghu_proxy PRIVATE /W4 /WX)
  target_compile_options(laghu PRIVATE /W4 /WX)
endif()

if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(laghu_proxy_test tests/proxy_test.c)
  target_link_libraries(laghu_proxy_test PRIVATE laghu_proxy)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_proxy_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(laghu_proxy_test PRIVATE /W4 /WX)
  endif()
  add_test(NAME laghu_proxy_test COMMAND laghu_proxy_test)
  add_test(NAME laghu_proxy_smoke
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/proxy_smoke.py
                   $<TARGET_FILE:laghu>
                   $<TARGET_FILE:laghu_runtime_cache_fixture>)
endif()
