# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SCRIPT OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED WORKLOAD)
  message(FATAL_ERROR "Laghu codec benchmark expectation requires SCRIPT BUILD_DIRECTORY WORKLOAD")
endif()

execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --workload "${WORKLOAD}"
    --warmup 1 --intervals 10
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0)
  message(FATAL_ERROR
    "Laghu codec benchmark expectation failed: workload=${WORKLOAD} output=${output}${diagnostics}")
endif()
string(JSON schema GET "${output}" schema_version)
string(JSON workload GET "${output}" workload)
string(JSON intervals GET "${output}" parameters intervals)
if(NOT schema STREQUAL "laghu-benchmark-v1" OR
    NOT workload STREQUAL WORKLOAD OR NOT intervals EQUAL 10)
  message(FATAL_ERROR
    "Laghu codec benchmark expectation failed: workload=${WORKLOAD} rule=schema")
endif()
