# SPDX-License-Identifier: AGPL-3.0-only
foreach(variable IN ITEMS SOURCE BUILD_DIRECTORY CXX CLANG_TIDY SCAN_BUILD CLANG_ANALYZER CPPCHECK
    CLANG_TIDY_CONFIG CLANG_ANALYZER_CONFIG CPPCHECK_CONFIG)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "Laghu static analysis requires ${variable}")
  endif()
endforeach()
if(NOT DEFINED ANALYSIS_TARGETS OR ANALYSIS_TARGETS STREQUAL "")
  message(FATAL_ERROR "Laghu static analysis requires ANALYSIS_TARGETS")
endif()
string(REPLACE "," ";" analysis_targets "${ANALYSIS_TARGETS}")

set(compile_commands "${BUILD_DIRECTORY}/compile_commands.json")
if(NOT EXISTS "${compile_commands}")
  message(FATAL_ERROR "Laghu static analysis failed: compile_commands_missing")
endif()
file(READ "${compile_commands}" compile_database)
string(JSON command_count LENGTH "${compile_database}")
set(owned_sources)
set(owned_command_indexes)
if(command_count GREATER 0)
  math(EXPR last_command "${command_count} - 1")
  foreach(index RANGE ${last_command})
    string(JSON source GET "${compile_database}" ${index} file)
    file(REAL_PATH "${source}" source_real)
    file(RELATIVE_PATH relative_source "${SOURCE}" "${source_real}")
    if(relative_source MATCHES "^(src|bench)/.*\\.(cpp|cc|cxx)$")
      list(APPEND owned_sources "${source_real}")
      list(APPEND owned_command_indexes "${index}")
    endif()
  endforeach()
endif()
list(REMOVE_DUPLICATES owned_sources)
if(owned_sources STREQUAL "")
  message(FATAL_ERROR "Laghu static analysis failed: owned_compile_entries_missing")
endif()
file(TO_CMAKE_PATH "${SOURCE}/src" owned_header_root)
string(REGEX REPLACE "([][+.*^$(){}|\\\\?])" "\\\\\\1" owned_header_root_regex "${owned_header_root}")
file(TO_CMAKE_PATH "${SOURCE}/bench" benchmark_header_root)
string(REGEX REPLACE "([][+.*^$(){}|\\\\?])" "\\\\\\1" benchmark_header_root_regex "${benchmark_header_root}")
set(owned_header_filter "^(${owned_header_root_regex}|${benchmark_header_root_regex})/.*")

execute_process(
  COMMAND "${CLANG_TIDY}" "--config-file=${CLANG_TIDY_CONFIG}"
    "--header-filter=${owned_header_filter}" --warnings-as-errors=* -p "${BUILD_DIRECTORY}"
    ${owned_sources}
  RESULT_VARIABLE tidy_result
  OUTPUT_VARIABLE tidy_output
  ERROR_VARIABLE tidy_diagnostics)
if(NOT tidy_result EQUAL 0)
  message(FATAL_ERROR "Laghu static analysis failed: tool=clang_tidy\n${tidy_output}${tidy_diagnostics}")
endif()

file(STRINGS "${CPPCHECK_CONFIG}" cppcheck_options)
list(FILTER cppcheck_options EXCLUDE REGEX "^[ \\t]*(#|$)")
execute_process(
  COMMAND "${CPPCHECK}" ${cppcheck_options} "--project=${compile_commands}"
    "--file-filter=${SOURCE}/src/*" "--file-filter=${SOURCE}/bench/*"
  RESULT_VARIABLE cppcheck_result
  OUTPUT_VARIABLE cppcheck_output
  ERROR_VARIABLE cppcheck_diagnostics)
if(NOT cppcheck_result EQUAL 0)
  message(FATAL_ERROR "Laghu static analysis failed: tool=cppcheck\n${cppcheck_output}${cppcheck_diagnostics}")
endif()

file(STRINGS "${CLANG_ANALYZER_CONFIG}" analyzer_options)
list(FILTER analyzer_options EXCLUDE REGEX "^[ \\t]*(#|$)")
get_filename_component(cxx_command "${CXX}" NAME)
foreach(index IN LISTS owned_command_indexes)
  string(JSON compile_command GET "${compile_database}" ${index} command)
  separate_arguments(compile_arguments NATIVE_COMMAND "${compile_command}")
  list(REMOVE_AT compile_arguments 0)
  execute_process(
    COMMAND "${SCAN_BUILD}" --exclude "${BUILD_DIRECTORY}/_deps" --use-cc "${CLANG_ANALYZER}"
      --use-c++ "${CXX}" ${analyzer_options} "${cxx_command}" ${compile_arguments}
    RESULT_VARIABLE analyzer_result
    OUTPUT_VARIABLE analyzer_output
    ERROR_VARIABLE analyzer_diagnostics)
  if(NOT analyzer_result EQUAL 0)
    message(FATAL_ERROR "Laghu static analysis failed: tool=clang_analyzer; source_index=${index}\n${analyzer_output}${analyzer_diagnostics}")
  endif()
endforeach()
