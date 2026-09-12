# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE)
  message(FATAL_ERROR "Laghu fuzz TSAN rejection expectation requires MODULE")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DMODULE=${MODULE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/FuzzTsanRejectedFixture.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz TSAN rejection expectation failed: rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "incompatible_sanitizer_profile=TSAN")
  message(FATAL_ERROR "Laghu fuzz TSAN rejection expectation failed: rejection=unrelated")
endif()
