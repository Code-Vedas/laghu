# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

if(WIN32)
  set(LAGHU_RUNTIME_SOURCE src/runtime_win32.c)
else()
  set(LAGHU_RUNTIME_SOURCE src/runtime_posix.c)
endif()

add_library(laghu_runtime STATIC ${LAGHU_RUNTIME_SOURCE})
add_library(Laghu::Runtime ALIAS laghu_runtime)

target_include_directories(laghu_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_runtime PUBLIC c_std_11)
target_link_libraries(laghu_runtime PUBLIC Laghu::Core)
set_target_properties(laghu_runtime PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_runtime_test tests/runtime_test.c)
  target_link_libraries(laghu_runtime_test PRIVATE Laghu::Runtime)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_runtime_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_runtime_test COMMAND laghu_runtime_test)
endif()
