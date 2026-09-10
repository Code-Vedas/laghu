# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED SCRIPT)
  message(FATAL_ERROR "Laghu test-wrapper expectation requires SCRIPT")
endif()
if(NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "Laghu test-wrapper expectation requires TEST_ROOT")
endif()
if(NOT DEFINED TEST_BUILD)
  message(FATAL_ERROR "Laghu test-wrapper expectation requires TEST_BUILD")
endif()

function(laghu_expect_test_wrapper expected_result)
  execute_process(
    COMMAND "${SCRIPT}" ${ARGN}
    RESULT_VARIABLE actual_result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT actual_result EQUAL expected_result)
    message(FATAL_ERROR
      "Laghu test-wrapper expectation failed: expected=${expected_result} actual=${actual_result} output=${output}${error}")
  endif()
endfunction()

function(laghu_expect_test_wrapper_failure)
  execute_process(
    COMMAND "${SCRIPT}" ${ARGN}
    RESULT_VARIABLE actual_result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(actual_result EQUAL 0)
    message(FATAL_ERROR
      "Laghu test-wrapper expectation failed: expected a failure output=${output}${error}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_ROOT}")
set(non_directory_build "${TEST_ROOT}/not-a-directory")
if(EXISTS "${non_directory_build}")
  if(IS_DIRECTORY "${non_directory_build}")
    message(FATAL_ERROR "Laghu test-wrapper fixture is unexpectedly a directory: ${non_directory_build}")
  endif()
  file(REMOVE "${non_directory_build}")
endif()
file(WRITE "${non_directory_build}" "fixture\n")

laghu_expect_test_wrapper(64)
laghu_expect_test_wrapper(64 --filter laghu)
laghu_expect_test_wrapper(64 --build "${TEST_ROOT}" --filter "")
laghu_expect_test_wrapper(64 --build "${TEST_ROOT}" --unknown)
laghu_expect_test_wrapper(66 --build "${non_directory_build}")
laghu_expect_test_wrapper_failure(
  --build "${TEST_BUILD}"
  --filter "^laghu\\.test_support\\.missing$")
