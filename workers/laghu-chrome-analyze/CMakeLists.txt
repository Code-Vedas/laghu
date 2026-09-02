# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_executable(laghu-chrome-analyze main.c)
target_link_libraries(laghu-chrome-analyze PRIVATE Laghu::Runtime)
target_compile_features(laghu-chrome-analyze PRIVATE c_std_11)

if(LAGHU_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(laghu_chrome_analyze_fixture tests/analysis_fixture.c)
  target_link_libraries(laghu_chrome_analyze_fixture PRIVATE Laghu::Runtime)
  target_compile_features(laghu_chrome_analyze_fixture PRIVATE c_std_11)
  add_executable(laghu_chrome_analyze_root_test main.c)
  target_link_libraries(laghu_chrome_analyze_root_test PRIVATE Laghu::Runtime)
  target_compile_features(laghu_chrome_analyze_root_test PRIVATE c_std_11)
  target_compile_definitions(laghu_chrome_analyze_root_test PRIVATE LAGHU_CHROME_ANALYZE_TEST_EUID=0)
  add_test(NAME laghu_chrome_analyze_startup_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_test.py
      $<TARGET_FILE:laghu-chrome-analyze>
      $<TARGET_FILE:laghu_chrome_analyze_root_test>
      ${CMAKE_CURRENT_SOURCE_DIR}/main.c
      ${CMAKE_SOURCE_DIR}/packaging/systemd/laghu-chrome-analyze.service)
  find_program(LAGHU_CHROME_EXECUTABLE NAMES chromium chromium-browser google-chrome)
  if(LAGHU_CHROME_EXECUTABLE)
    add_test(NAME laghu_chrome_analyze_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_test.py
        $<TARGET_FILE:laghu-chrome-analyze>
        $<TARGET_FILE:laghu_chrome_analyze_fixture>
        ${LAGHU_CHROME_EXECUTABLE})
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-chrome-analyze PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(LAGHU_BUILD_TESTS)
    target_compile_options(laghu_chrome_analyze_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(laghu_chrome_analyze_root_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
