# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECT_FAIL)
  message(FATAL_ERROR "ExpectDagConfigure requires LAGHU_SOURCE, SCENARIO, and EXPECT_FAIL")
endif()

set(binary_directory "${CMAKE_CURRENT_BINARY_DIR}/dag-${SCENARIO}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${LAGHU_SOURCE}/tests/dag/fixture"
    -B "${binary_directory}"
    "-DLAGHU_SOURCE=${LAGHU_SOURCE}"
    "-DSCENARIO=${SCENARIO}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)

if(EXPECT_FAIL AND result EQUAL 0)
  message(FATAL_ERROR "Laghu DAG fixture unexpectedly configured: scenario=${SCENARIO}")
endif()
if(NOT EXPECT_FAIL AND NOT result EQUAL 0)
  message(FATAL_ERROR
    "Laghu DAG fixture failed: scenario=${SCENARIO}\n${output}\n${diagnostics}")
endif()
if(DEFINED EXPECT_TEXT)
  set(combined "${output}\n${diagnostics}")
  # CMake wraps long diagnostic lines. Compare the semantic fields after
  # normalizing presentation whitespace rather than depending on wrapping.
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_combined "${combined}")
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_expected "${EXPECT_TEXT}")
  string(FIND "${normalized_combined}" "${normalized_expected}" text_offset)
  if(text_offset EQUAL -1)
    message(FATAL_ERROR
      "Laghu DAG fixture diagnostic missing: scenario=${SCENARIO}; expected=${EXPECT_TEXT}\n${combined}")
  endif()
endif()
