# SPDX-License-Identifier: AGPL-3.0-only
foreach(variable IN ITEMS TOOL SOURCE WORK_DIRECTORY)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "Laghu static-analysis failure expectation requires ${variable}")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")

if(TOOL STREQUAL "clang_tidy")
  execute_process(
    COMMAND "${CLANG_TIDY}" "--config-file=${CLANG_TIDY_CONFIG}" --warnings-as-errors=*
      "${SOURCE}" -- -std=c++23
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  set(expected bugprone-narrowing-conversions)
elseif(TOOL STREQUAL "clang_analyzer")
  file(STRINGS "${CLANG_ANALYZER_CONFIG}" analyzer_options)
  list(FILTER analyzer_options EXCLUDE REGEX "^[ \\t]*(#|$)")
  execute_process(
    COMMAND "${SCAN_BUILD}" --use-cc "${CLANG_ANALYZER}" --use-c++ "${CLANG_ANALYZER}" ${analyzer_options}
      -o "${WORK_DIRECTORY}" "${CLANG_ANALYZER}" -std=c++23 -c "${SOURCE}"
      -o "${WORK_DIRECTORY}/fixture.o"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  set(expected "Dereference of null pointer")
elseif(TOOL STREQUAL "cppcheck")
  file(STRINGS "${CPPCHECK_CONFIG}" cppcheck_options)
  list(FILTER cppcheck_options EXCLUDE REGEX "^[ \\t]*(#|$)")
  execute_process(
    COMMAND "${CPPCHECK}" ${cppcheck_options} "${SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  set(expected uninitvar)
else()
  message(FATAL_ERROR "Laghu static-analysis failure expectation failed: tool=unknown")
endif()
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu static-analysis failure expectation failed: tool=${TOOL}; rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "${expected}")
  message(FATAL_ERROR "Laghu static-analysis failure expectation failed: tool=${TOOL}; diagnostic=unrelated")
endif()
