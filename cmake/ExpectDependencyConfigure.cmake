# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED CMAKE_CXX_COMPILER OR
    NOT DEFINED CMAKE_CXX_FLAGS OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECT_FAIL)
  message(FATAL_ERROR "ExpectDependencyConfigure requires LAGHU_SOURCE, CMAKE_CXX_COMPILER, CMAKE_CXX_FLAGS, SCENARIO, and EXPECT_FAIL")
endif()

set(binary_directory "${CMAKE_CURRENT_BINARY_DIR}/dependency-${SCENARIO}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${LAGHU_SOURCE}/tests/dependencies/fixture"
    -B "${binary_directory}"
    "-DLAGHU_SOURCE=${LAGHU_SOURCE}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_CXX_SCAN_FOR_MODULES=ON"
    "-DSCENARIO=${SCENARIO}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_diagnostics)

set(combined "${configure_output}\n${configure_diagnostics}")
if(configure_result EQUAL 0 AND (SCENARIO STREQUAL "positive_symbol" OR SCENARIO STREQUAL "capability_mismatch"))
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${binary_directory}" --target laghu_dependency_fixture_probe
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_diagnostics)
  string(APPEND combined "\n${build_output}\n${build_diagnostics}")
  set(result "${build_result}")
else()
  set(result "${configure_result}")
endif()

if(EXPECT_FAIL AND result EQUAL 0)
  message(FATAL_ERROR "Laghu dependency fixture unexpectedly passed: scenario=${SCENARIO}")
endif()
if(NOT EXPECT_FAIL AND NOT result EQUAL 0)
  message(FATAL_ERROR "Laghu dependency fixture failed: scenario=${SCENARIO}\n${combined}")
endif()
if(NOT EXPECT_TEXT STREQUAL "")
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_combined "${combined}")
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_expected "${EXPECT_TEXT}")
  string(FIND "${normalized_combined}" "${normalized_expected}" text_offset)
  if(text_offset EQUAL -1)
    message(FATAL_ERROR "Laghu dependency fixture diagnostic missing: scenario=${SCENARIO}; expected=${EXPECT_TEXT}\n${combined}")
  endif()
endif()
