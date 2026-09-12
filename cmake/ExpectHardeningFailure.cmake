# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE OR NOT DEFINED FEATURE)
  message(FATAL_ERROR "Laghu hardening failure expectation requires MODULE and FEATURE")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DMODULE=${MODULE}" "-DFEATURE=${FEATURE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/HardeningFailureFixture.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu hardening failure expectation failed: feature=${FEATURE}; rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "feature=${FEATURE}" OR NOT combined MATCHES "required=")
  message(FATAL_ERROR "Laghu hardening failure expectation failed: feature=${FEATURE}; rejection=unrelated")
endif()
