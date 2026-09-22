# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SOURCE OR NOT DEFINED BINARY OR NOT DEFINED CXX)
  message(FATAL_ERROR "Laghu single-config generator expectation requires SOURCE BINARY and CXX")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}" -G "Ninja Multi-Config"
    "-DCMAKE_CXX_COMPILER=${CXX}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu single-config generator expectation failed: rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "single-config Ninja generator")
  message(FATAL_ERROR "Laghu single-config generator expectation failed: rejection=unrelated")
endif()
