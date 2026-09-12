# SPDX-License-Identifier: AGPL-3.0-only
foreach(variable IN ITEMS CLANG_TIDY SCAN_BUILD CLANG_ANALYZER CPPCHECK CLANG_TIDY_CONFIG
    CPPCHECK_CONFIG LAGHU_SOURCE WORK_DIRECTORY)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "Laghu static-analysis ownership expectation requires ${variable}")
  endif()
endforeach()

function(laghu_escape_regex output input)
  string(REGEX REPLACE "([][+.*^$(){}|\\\\?])" "\\\\\\1" escaped "${input}")
  set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

file(TO_CMAKE_PATH "${LAGHU_SOURCE}/src" owned_header_root)
laghu_escape_regex(owned_header_root_regex "${owned_header_root}")
set(owned_header_filter "^${owned_header_root_regex}/.*")
set(ownership_root "${LAGHU_SOURCE}/tests/static-analysis/ownership")
set(ownership_source "${ownership_root}/owned.cpp")

execute_process(
  COMMAND "${CLANG_TIDY}" "--config-file=${CLANG_TIDY_CONFIG}"
    "--header-filter=(^|.*/)src/.*" --warnings-as-errors=* "${ownership_source}" --
    -std=c++23 "-I${ownership_root}"
  RESULT_VARIABLE broad_result
  OUTPUT_VARIABLE broad_output
  ERROR_VARIABLE broad_diagnostics)
set(broad_combined "${broad_output}${broad_diagnostics}")
if(broad_result EQUAL 0 OR NOT broad_combined MATCHES "unowned.hpp")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: clang_tidy_fixture=invalid")
endif()

execute_process(
  COMMAND "${CLANG_TIDY}" "--config-file=${CLANG_TIDY_CONFIG}"
    "--header-filter=${owned_header_filter}" --warnings-as-errors=* "${ownership_source}" --
    -std=c++23 "-I${ownership_root}"
  RESULT_VARIABLE owned_tidy_result
  OUTPUT_VARIABLE owned_tidy_output
  ERROR_VARIABLE owned_tidy_diagnostics)
set(owned_tidy_combined "${owned_tidy_output}${owned_tidy_diagnostics}")
if(NOT owned_tidy_result EQUAL 0 OR owned_tidy_combined MATCHES "unowned.hpp")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: clang_tidy_boundary=leaked")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}/src" "${WORK_DIRECTORY}/_deps/vendor-src/src")
file(WRITE "${WORK_DIRECTORY}/src/owned.cpp" "int main() { return 0; }\n")
file(WRITE "${WORK_DIRECTORY}/_deps/vendor-src/src/cppcheck.cpp"
  "int unowned_cppcheck() { int value; return value; }\n")
file(WRITE "${WORK_DIRECTORY}/_deps/vendor-src/src/analyzer.cpp"
  "int unowned_analyzer() { int* value = nullptr; return *value; }\n")
file(STRINGS "${CPPCHECK_CONFIG}" cppcheck_options)
list(FILTER cppcheck_options EXCLUDE REGEX "^[ \\t]*(#|$)")

execute_process(
  COMMAND "${CPPCHECK}" ${cppcheck_options}
    "${WORK_DIRECTORY}/_deps/vendor-src/src/cppcheck.cpp"
  RESULT_VARIABLE unowned_cppcheck_result
  OUTPUT_VARIABLE unowned_cppcheck_output
  ERROR_VARIABLE unowned_cppcheck_diagnostics)
set(unowned_cppcheck_combined "${unowned_cppcheck_output}${unowned_cppcheck_diagnostics}")
if(unowned_cppcheck_result EQUAL 0 OR NOT unowned_cppcheck_combined MATCHES "uninitvar")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: cppcheck_fixture=invalid")
endif()

execute_process(
  COMMAND "${CPPCHECK}" ${cppcheck_options} "--file-filter=${WORK_DIRECTORY}/src/*"
    "${WORK_DIRECTORY}/src/owned.cpp" "${WORK_DIRECTORY}/_deps/vendor-src/src/cppcheck.cpp"
  RESULT_VARIABLE owned_cppcheck_result
  OUTPUT_VARIABLE owned_cppcheck_output
  ERROR_VARIABLE owned_cppcheck_diagnostics)
set(owned_cppcheck_combined "${owned_cppcheck_output}${owned_cppcheck_diagnostics}")
if(NOT owned_cppcheck_result EQUAL 0 OR owned_cppcheck_combined MATCHES "cppcheck.cpp")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: cppcheck_boundary=leaked")
endif()

execute_process(
  COMMAND "${SCAN_BUILD}" --use-cc "${CLANG_ANALYZER}" --use-c++ "${CLANG_ANALYZER}"
    --status-bugs -o "${WORK_DIRECTORY}/scan-owned" "${CLANG_ANALYZER}" -std=c++23 -c
    "${WORK_DIRECTORY}/src/owned.cpp" -o "${WORK_DIRECTORY}/owned.o"
  RESULT_VARIABLE owned_analyzer_result
  OUTPUT_VARIABLE owned_analyzer_output
  ERROR_VARIABLE owned_analyzer_diagnostics)
if(NOT owned_analyzer_result EQUAL 0)
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: clang_analyzer_owned")
endif()

execute_process(
  COMMAND "${SCAN_BUILD}" --use-cc "${CLANG_ANALYZER}" --use-c++ "${CLANG_ANALYZER}"
    --status-bugs -o "${WORK_DIRECTORY}/scan-unowned" "${CLANG_ANALYZER}" -std=c++23 -c
    "${WORK_DIRECTORY}/_deps/vendor-src/src/analyzer.cpp" -o "${WORK_DIRECTORY}/analyzer.o"
  RESULT_VARIABLE unowned_analyzer_result
  OUTPUT_VARIABLE unowned_analyzer_output
  ERROR_VARIABLE unowned_analyzer_diagnostics)
set(unowned_analyzer_combined "${unowned_analyzer_output}${unowned_analyzer_diagnostics}")
if(unowned_analyzer_result EQUAL 0 OR NOT unowned_analyzer_combined MATCHES "Dereference of null pointer")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: clang_analyzer_fixture=invalid")
endif()
