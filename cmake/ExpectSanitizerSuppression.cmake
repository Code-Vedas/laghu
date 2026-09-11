# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MANIFEST)
  message(FATAL_ERROR "Laghu sanitizer suppression expectation requires MANIFEST")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${MANIFEST}"
    -P "${CMAKE_CURRENT_LIST_DIR}/ValidateSanitizerSuppression.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu sanitizer suppression expectation failed: manifest=${MANIFEST}; rejection=missing")
endif()
