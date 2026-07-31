# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

if(WIN32)
  set(LAGHU_RUNTIME_SOURCE src/runtime_win32.c)
else()
  set(LAGHU_RUNTIME_SOURCE src/runtime_posix.c)
endif()

add_library(laghu_runtime STATIC ${LAGHU_RUNTIME_SOURCE} src/rum.c src/catalog.c src/html.c src/html_head.c src/html_hints.c src/css_runtime.c src/css_catalog.c src/css_combine.c src/css_imports.c src/css_markup.c src/critical_css.c src/font_provider.c src/javascript.c src/javascript_defer.c src/instrumentation.c src/beacon.c)
add_library(Laghu::Runtime ALIAS laghu_runtime)

target_include_directories(laghu_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_runtime PUBLIC c_std_11)
target_link_libraries(laghu_runtime PUBLIC Laghu::Core Laghu::Image ${CMAKE_DL_LIBS})
set_target_properties(laghu_runtime PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LAGHU_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(laghu_runtime_fuzz tests/runtime_fuzz.c)
  target_link_libraries(laghu_runtime_fuzz PRIVATE Laghu::Runtime)
  target_compile_options(laghu_runtime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(laghu_runtime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_runtime_cache_fixture tests/cache_fixture.c)
  target_link_libraries(laghu_runtime_cache_fixture PRIVATE Laghu::Runtime)
  add_executable(laghu_runtime_test tests/runtime_test.c)
  target_link_libraries(laghu_runtime_test PRIVATE Laghu::Runtime)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_runtime_test COMMAND laghu_runtime_test)
endif()
