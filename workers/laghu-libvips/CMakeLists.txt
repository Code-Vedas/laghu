# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_executable(laghu-libvips main.c)
target_link_libraries(laghu-libvips PRIVATE Laghu::Image Laghu::Runtime)
target_compile_definitions(
  laghu-libvips PRIVATE LAGHU_TEST_HOOKS=$<BOOL:${LAGHU_BUILD_TESTS}>
)

if(LAGHU_BUILD_TESTS AND LAGHU_HAVE_VIPS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(
    LAGHU_VIPS_EXECUTABLE
    NAMES vips vips.exe
    HINTS
      "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/tools/libvips"
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/libvips"
    REQUIRED
  )
  add_test(
    NAME laghu_libvips_integration_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_test.py
            $<TARGET_FILE:laghu-libvips> ${LAGHU_VIPS_EXECUTABLE}
            $<TARGET_FILE:laghu_runtime_cache_fixture>
  )
  if(WIN32)
    set_tests_properties(
      laghu_libvips_integration_test
      PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin"
    )
  endif()
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
