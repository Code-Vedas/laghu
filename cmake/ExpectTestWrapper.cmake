# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED SCRIPT)
  message(FATAL_ERROR "Laghu test-wrapper expectation requires SCRIPT")
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

laghu_expect_test_wrapper(64)
laghu_expect_test_wrapper(64 --filter laghu)
laghu_expect_test_wrapper(64 --build /tmp/laghu-wrapper-unused --filter "")
laghu_expect_test_wrapper(64 --build /tmp/laghu-wrapper-unused --unknown)
laghu_expect_test_wrapper(66 --build /tmp/laghu-wrapper-missing)
