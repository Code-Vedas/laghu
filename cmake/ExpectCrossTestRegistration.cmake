# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED FIXTURE_BINARY)
  message(FATAL_ERROR "Laghu cross test registration expectation requires LAGHU_SOURCE and FIXTURE_BINARY")
endif()

set(fixture_source "${LAGHU_SOURCE}/tests/configure/cross-test-registration")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${fixture_source}" -B "${FIXTURE_BINARY}" -G Ninja
    "-DLAGHU_SOURCE=${LAGHU_SOURCE}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "Laghu cross test registration expectation failed: configure_failed output=${configure_output}${configure_error}")
endif()

set(test_file "${FIXTURE_BINARY}/CTestTestfile.cmake")
if(NOT EXISTS "${test_file}")
  message(FATAL_ERROR "Laghu cross test registration expectation failed: test_file_missing")
endif()
file(READ "${test_file}" registered_tests)
if(NOT registered_tests MATCHES "laghu.fixture.scripted" OR
    registered_tests MATCHES "laghu.fixture.native")
  message(FATAL_ERROR "Laghu cross test registration expectation failed: target_execution_registered")
endif()
