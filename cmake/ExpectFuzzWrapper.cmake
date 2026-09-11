# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SCRIPT OR NOT DEFINED TEST_BUILD OR NOT DEFINED CORPUS)
  message(FATAL_ERROR "Laghu fuzz wrapper expectation requires SCRIPT TEST_BUILD and CORPUS")
endif()

execute_process(
  COMMAND "${SCRIPT}" --build "${TEST_BUILD}" --target binary-envelope --corpus "${CORPUS}" --runs 1
  RESULT_VARIABLE positive_result
  OUTPUT_VARIABLE positive_output
  ERROR_VARIABLE positive_diagnostics)
if(NOT positive_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz wrapper expectation failed: positive=${positive_output}${positive_diagnostics}")
endif()

foreach(case IN ITEMS unknown-target invalid-runs missing-corpus)
  if(case STREQUAL "unknown-target")
    set(arguments --build "${TEST_BUILD}" --target unknown --corpus "${CORPUS}")
  elseif(case STREQUAL "invalid-runs")
    set(arguments --build "${TEST_BUILD}" --target binary-envelope --corpus "${CORPUS}" --runs 0)
  else()
    set(arguments --build "${TEST_BUILD}" --target binary-envelope --corpus "${CORPUS}/missing")
  endif()
  execute_process(COMMAND "${SCRIPT}" ${arguments}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  if(case STREQUAL "missing-corpus")
    set(expected_exit 66)
  else()
    set(expected_exit 64)
  endif()
  if(NOT result EQUAL expected_exit)
    message(FATAL_ERROR "Laghu fuzz wrapper expectation failed: case=${case}; expected_exit=${expected_exit}; actual_exit=${result}; output=${output}${diagnostics}")
  endif()
endforeach()
