# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_executable(laghu-libvips main.c)
target_link_libraries(laghu-libvips PRIVATE Laghu::Image Laghu::Runtime)
target_compile_definitions(
  laghu-libvips PRIVATE LAGHU_TEST_HOOKS=$<BOOL:${LAGHU_BUILD_TESTS}>
)

if(LAGHU_BUILD_TESTS AND LAGHU_HAVE_VIPS AND NOT WIN32)
  add_test(
    NAME laghu_libvips_integration_test
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_test.sh
            $<TARGET_FILE:laghu-libvips>
  )
endif()
if(LAGHU_BUILD_TESTS AND WIN32)
  add_test(NAME laghu_libvips_windows_probe
           COMMAND $<TARGET_FILE:laghu-libvips> --probe)
  if(NOT LAGHU_HAVE_VIPS)
    set_tests_properties(laghu_libvips_windows_probe PROPERTIES WILL_FAIL TRUE)
  endif()
endif()
target_compile_features(laghu-libvips PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-libvips PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
