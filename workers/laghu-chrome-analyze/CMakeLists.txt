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
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(laghu_chrome_analyze_old_bwrap_test main.c)
    target_link_libraries(laghu_chrome_analyze_old_bwrap_test PRIVATE Laghu::Runtime)
    target_compile_features(laghu_chrome_analyze_old_bwrap_test PRIVATE c_std_11)
    target_compile_definitions(laghu_chrome_analyze_old_bwrap_test PRIVATE LAGHU_CHROME_ANALYZE_TEST_BWRAP_VERSION=\"0.6.3\")
    add_test(NAME laghu_chrome_analyze_old_bwrap_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/bwrap_version_test.py
        $<TARGET_FILE:laghu_chrome_analyze_old_bwrap_test>)
    set_tests_properties(laghu_chrome_analyze_old_bwrap_test PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  add_test(NAME laghu_chrome_analyze_startup_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_test.py
      $<TARGET_FILE:laghu-chrome-analyze>
      $<TARGET_FILE:laghu_chrome_analyze_root_test>
      ${CMAKE_CURRENT_SOURCE_DIR}/main.c
      ${CMAKE_SOURCE_DIR}/packaging/systemd/laghu-chrome-analyze.service)
  set_tests_properties(laghu_chrome_analyze_startup_test PROPERTIES SKIP_RETURN_CODE 77)
  find_program(
    LAGHU_CHROME_RUNTIME_EXECUTABLE
    NAMES chromium chromium-browser
    PATHS /usr/lib/chromium /usr/lib/chromium-browser /usr/lib64/chromium /usr/lib64/chromium-browser
    NO_DEFAULT_PATH)
  find_program(LAGHU_CHROME_EXECUTABLE NAMES chromium chromium-browser google-chrome)
  if(LAGHU_CHROME_RUNTIME_EXECUTABLE)
    set(LAGHU_CHROME_EXECUTABLE ${LAGHU_CHROME_RUNTIME_EXECUTABLE})
  endif()
  if(LAGHU_CHROME_EXECUTABLE)
    add_test(NAME laghu_chrome_analyze_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis_test.py
        $<TARGET_FILE:laghu-chrome-analyze>
        $<TARGET_FILE:laghu_chrome_analyze_fixture>
        ${LAGHU_CHROME_EXECUTABLE})
    add_test(NAME laghu_chrome_analyze_network_isolation_test
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_isolation_test.py
        $<TARGET_FILE:laghu-chrome-analyze>
        $<TARGET_FILE:laghu_chrome_analyze_fixture>
        ${LAGHU_CHROME_EXECUTABLE})
    set_tests_properties(laghu_chrome_analyze_test laghu_chrome_analyze_network_isolation_test PROPERTIES
      SKIP_RETURN_CODE 77
      TIMEOUT 30)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-chrome-analyze PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(LAGHU_BUILD_TESTS)
    target_compile_options(laghu_chrome_analyze_fixture PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(laghu_chrome_analyze_root_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(TARGET laghu_chrome_analyze_old_bwrap_test)
      target_compile_options(laghu_chrome_analyze_old_bwrap_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
endif()
