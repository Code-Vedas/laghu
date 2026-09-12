# SPDX-License-Identifier: AGPL-3.0-only
foreach(variable IN ITEMS SOURCE BUILD_DIRECTORY CXX CLANG_TIDY SCAN_BUILD CLANG_ANALYZER CPPCHECK
    CLANG_TIDY_CONFIG CLANG_ANALYZER_CONFIG CPPCHECK_CONFIG)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "Laghu static analysis requires ${variable}")
  endif()
endforeach()

set(compile_commands "${BUILD_DIRECTORY}/compile_commands.json")
if(NOT EXISTS "${compile_commands}")
  message(FATAL_ERROR "Laghu static analysis failed: compile_commands_missing")
endif()
file(READ "${compile_commands}" compile_database)
string(JSON command_count LENGTH "${compile_database}")
set(owned_sources)
if(command_count GREATER 0)
  math(EXPR last_command "${command_count} - 1")
  foreach(index RANGE ${last_command})
    string(JSON source GET "${compile_database}" ${index} file)
    file(REAL_PATH "${source}" source_real)
    file(RELATIVE_PATH relative_source "${SOURCE}" "${source_real}")
    if(relative_source MATCHES "^src/.*\\.(cpp|cc|cxx)$")
      list(APPEND owned_sources "${source_real}")
    endif()
  endforeach()
endif()
list(REMOVE_DUPLICATES owned_sources)
if(owned_sources STREQUAL "")
  message(FATAL_ERROR "Laghu static analysis failed: owned_compile_entries_missing")
endif()
file(TO_CMAKE_PATH "${SOURCE}/src" owned_header_root)
string(REGEX REPLACE "([][+.*^$(){}|\\\\?])" "\\\\\\1" owned_header_root_regex "${owned_header_root}")
set(owned_header_filter "^${owned_header_root_regex}/.*")

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
    "--file-filter=${SOURCE}/src/*"
  RESULT_VARIABLE cppcheck_result
  OUTPUT_VARIABLE cppcheck_output
  ERROR_VARIABLE cppcheck_diagnostics)
if(NOT cppcheck_result EQUAL 0)
  message(FATAL_ERROR "Laghu static analysis failed: tool=cppcheck\n${cppcheck_output}${cppcheck_diagnostics}")
endif()

set(analyzer_build "${BUILD_DIRECTORY}/analysis/clang-analyzer")
file(REMOVE_RECURSE "${analyzer_build}")
set(analyzer_configure_arguments
  -S "${SOURCE}"
  -B "${analyzer_build}"
  -G Ninja
  "-DCMAKE_CXX_COMPILER=${CXX}"
  -DCMAKE_BUILD_TYPE=Release
  -DLAGHU_BUILD_PROFILE=MINIMAL)
if(DEFINED CXX_FLAGS AND NOT CXX_FLAGS STREQUAL "")
  list(APPEND analyzer_configure_arguments "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${analyzer_configure_arguments}
  RESULT_VARIABLE analyzer_configure_result
  OUTPUT_VARIABLE analyzer_configure_output
  ERROR_VARIABLE analyzer_configure_diagnostics)
if(NOT analyzer_configure_result EQUAL 0)
  message(FATAL_ERROR "Laghu static analysis failed: tool=clang_analyzer_configure\n${analyzer_configure_output}${analyzer_configure_diagnostics}")
endif()
file(STRINGS "${CLANG_ANALYZER_CONFIG}" analyzer_options)
list(FILTER analyzer_options EXCLUDE REGEX "^[ \\t]*(#|$)")
execute_process(
  COMMAND "${SCAN_BUILD}" --use-cc "${CLANG_ANALYZER}" --use-c++ "${CXX}" ${analyzer_options}
    "${CMAKE_COMMAND}" --build "${analyzer_build}" --target laghu_core laghu_os laghu
  RESULT_VARIABLE analyzer_result
  OUTPUT_VARIABLE analyzer_output
  ERROR_VARIABLE analyzer_diagnostics)
if(NOT analyzer_result EQUAL 0)
  message(FATAL_ERROR "Laghu static analysis failed: tool=clang_analyzer\n${analyzer_output}${analyzer_diagnostics}")
endif()
