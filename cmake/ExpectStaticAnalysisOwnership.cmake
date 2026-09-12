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

function(laghu_json_escape output input)
  set(value "${input}")
  string(REPLACE "\\" "\\\\" value "${value}")
  string(REPLACE "\"" "\\\"" value "${value}")
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(laghu_write_mixed_compile_database owned vendor)
  set(production_build "${WORK_DIRECTORY}/production-build")
  file(MAKE_DIRECTORY "${production_build}")
  foreach(path IN ITEMS "${production_build}" "${owned}" "${vendor}")
    laghu_json_escape(path_json "${path}")
    if(path STREQUAL "${production_build}")
      set(build_json "${path_json}")
    elseif(path STREQUAL "${owned}")
      set(owned_json "${path_json}")
    else()
      set(vendor_json "${path_json}")
    endif()
  endforeach()
  set(owned_command "${CLANG_ANALYZER} -std=c++23 -c ${owned} -o ${production_build}/owned.o")
  set(vendor_command "${CLANG_ANALYZER} -std=c++23 -c ${vendor} -o ${production_build}/vendor.o")
  laghu_json_escape(owned_command_json "${owned_command}")
  laghu_json_escape(vendor_command_json "${vendor_command}")
  file(WRITE "${production_build}/compile_commands.json"
    "[{\"directory\":\"${build_json}\",\"command\":\"${owned_command_json}\",\"file\":\"${owned_json}\"},"
    "{\"directory\":\"${build_json}\",\"command\":\"${vendor_command_json}\",\"file\":\"${vendor_json}\"}]")
endfunction()

function(laghu_run_production_analysis expected_failure)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE=${WORK_DIRECTORY}"
      "-DBUILD_DIRECTORY=${WORK_DIRECTORY}/production-build"
      "-DCXX=${CLANG_ANALYZER}"
      "-DCLANG_TIDY=${CLANG_TIDY}"
      "-DSCAN_BUILD=${SCAN_BUILD}"
      "-DCLANG_ANALYZER=${CLANG_ANALYZER}"
      "-DCPPCHECK=${CPPCHECK}"
      "-DCLANG_TIDY_CONFIG=${CLANG_TIDY_CONFIG}"
      "-DCLANG_ANALYZER_CONFIG=${LAGHU_SOURCE}/cmake/static-analysis/clang-analyzer-19.txt"
      "-DCPPCHECK_CONFIG=${CPPCHECK_CONFIG}"
      "-DANALYSIS_TARGETS=laghu_owned"
      -P "${LAGHU_SOURCE}/cmake/RunStaticAnalysis.cmake"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  set(production_output "${output}${diagnostics}" PARENT_SCOPE)
  if(expected_failure AND result EQUAL 0)
    message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: owned_defect=missed")
  endif()
  if(NOT expected_failure AND NOT result EQUAL 0)
    message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: vendor_defect=leaked\n${production_output}")
  endif()
endfunction()

set(production_source_root "${WORK_DIRECTORY}/src")
set(production_vendor_root "${WORK_DIRECTORY}/_deps/vendor-src/src")
file(MAKE_DIRECTORY "${production_source_root}" "${production_vendor_root}")
set(production_owned "${production_source_root}/owned.cpp")
set(production_vendor "${production_vendor_root}/vendor.cpp")

file(WRITE "${production_owned}" "int main() { return 0; }\n")
file(WRITE "${production_vendor}"
  "int laghu_vendor_defect() { int* value = nullptr; return *value; }\n")
laghu_write_mixed_compile_database("${production_owned}" "${production_vendor}")
laghu_run_production_analysis(FALSE)

file(WRITE "${production_owned}"
  "int main() { int* value = nullptr; return *value; }\n")
file(WRITE "${production_vendor}" "int laghu_vendor_clean() { return 0; }\n")
laghu_write_mixed_compile_database("${production_owned}" "${production_vendor}")
laghu_run_production_analysis(TRUE)
if(NOT production_output MATCHES "tool=(clang_tidy|cppcheck|clang_analyzer)")
  message(FATAL_ERROR "Laghu static-analysis ownership expectation failed: owned_defect=unrelated")
endif()
