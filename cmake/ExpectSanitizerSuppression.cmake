# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MANIFEST OR NOT DEFINED EXPECTED_DIAGNOSTIC)
  message(FATAL_ERROR "Laghu sanitizer suppression expectation requires MANIFEST and EXPECTED_DIAGNOSTIC")
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
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "${EXPECTED_DIAGNOSTIC}")
  message(FATAL_ERROR "Laghu sanitizer suppression expectation failed: manifest=${MANIFEST}; diagnostic=${EXPECTED_DIAGNOSTIC}; rejection=unrelated")
endif()
