# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE)
  message(FATAL_ERROR "Laghu hardening unsupported expectation requires MODULE")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DMODULE=${MODULE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/HardeningUnsupportedFixture.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Laghu hardening unsupported expectation failed: ${output}${diagnostics}")
endif()
