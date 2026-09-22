# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED EXECUTABLE OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "Laghu sanitizer fixture expectation requires EXECUTABLE and EXPECTED")
endif()

set(command "${CMAKE_COMMAND}" -E env)
foreach(environment IN ITEMS "${ENVIRONMENT_1}" "${ENVIRONMENT_2}")
  if(NOT environment STREQUAL "")
    list(APPEND command "${environment}")
  endif()
endforeach()
list(APPEND command "${EXECUTABLE}")
execute_process(COMMAND ${command}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
set(combined "${output}${diagnostics}")
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu sanitizer fixture expectation failed: fixture=${EXPECTED}; result=success")
endif()
if(EXPECTED STREQUAL "heap_misuse" AND
    NOT combined MATCHES "ERROR: AddressSanitizer: heap-buffer-overflow")
  message(FATAL_ERROR "Laghu sanitizer fixture expectation failed: fixture=heap_misuse; report=missing_heap_buffer_overflow")
elseif(EXPECTED STREQUAL "undefined_behavior" AND
    NOT combined MATCHES "runtime error: signed integer overflow")
  message(FATAL_ERROR "Laghu sanitizer fixture expectation failed: fixture=undefined_behavior; report=missing_undefined_sanitizer")
elseif(EXPECTED STREQUAL "data_race" AND
    NOT combined MATCHES "WARNING: ThreadSanitizer: data race")
  message(FATAL_ERROR "Laghu sanitizer fixture expectation failed: fixture=data_race; report=missing_data_race")
endif()
