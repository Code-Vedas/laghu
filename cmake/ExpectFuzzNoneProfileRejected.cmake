# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE)
  message(FATAL_ERROR "Laghu fuzz NONE-profile rejection expectation requires MODULE")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DMODULE=${MODULE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/FuzzNoneProfileRejectedFixture.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz NONE-profile rejection expectation failed: rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "requires=sanitizer_profile_ASAN_UBSAN")
  message(FATAL_ERROR "Laghu fuzz NONE-profile rejection expectation failed: rejection=unrelated")
endif()
