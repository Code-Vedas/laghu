# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_library(laghu_http STATIC src/http.c)
add_library(Laghu::Http ALIAS laghu_http)

target_include_directories(laghu_http
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_http PUBLIC c_std_11)
target_link_libraries(laghu_http PUBLIC Laghu::Core Laghu::Image Laghu::Runtime)
set_target_properties(laghu_http PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_http PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(laghu_http PRIVATE /W4 /WX)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_http_test tests/http_test.c)
  target_link_libraries(laghu_http_test PRIVATE Laghu::Http)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_http_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(laghu_http_test PRIVATE /W4 /WX)
  endif()
  add_test(NAME laghu_http_test COMMAND laghu_http_test)
endif()
