# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_STATIC_ANALYSIS OFF CACHE BOOL "Enable Laghu-owned static analysis targets")

function(laghu_collect_static_analysis_targets output)
  get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  set(analysis_targets)
  foreach(target IN LISTS targets)
    get_target_property(target_sources "${target}" SOURCES)
    if(target_sources STREQUAL "target_sources-NOTFOUND")
      continue()
    endif()
    foreach(source IN LISTS target_sources)
      if(NOT source MATCHES "\\.(cpp|cc|cxx)$")
        continue()
      endif()
      get_filename_component(source_absolute "${source}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
      file(RELATIVE_PATH source_relative "${CMAKE_SOURCE_DIR}" "${source_absolute}")
      if(source_relative MATCHES "^(src|bench)/")
        list(APPEND analysis_targets "${target}")
        break()
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES analysis_targets)
  list(SORT analysis_targets)
  set(${output} "${analysis_targets}" PARENT_SCOPE)
endfunction()

function(laghu_configure_static_analysis)
  if(NOT LAGHU_STATIC_ANALYSIS)
    return()
  endif()
  find_program(clang_tidy NAMES clang-tidy-19 clang-tidy REQUIRED)
  find_program(scan_build NAMES scan-build-19 scan-build REQUIRED)
  find_program(clang_analyzer NAMES clang-19 clang REQUIRED)
  find_program(cppcheck NAMES cppcheck REQUIRED)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for Laghu analysis" FORCE)
  set(LAGHU_CLANG_TIDY_EXECUTABLE "${clang_tidy}" CACHE INTERNAL "Laghu clang-tidy executable")
  set(LAGHU_SCAN_BUILD_EXECUTABLE "${scan_build}" CACHE INTERNAL "Laghu scan-build executable")
  set(LAGHU_CLANG_ANALYZER_EXECUTABLE "${clang_analyzer}" CACHE INTERNAL "Laghu clang analyzer executable")
  set(LAGHU_CPPCHECK_EXECUTABLE "${cppcheck}" CACHE INTERNAL "Laghu cppcheck executable")
endfunction()

function(laghu_add_static_analysis_target)
  if(NOT LAGHU_STATIC_ANALYSIS)
    return()
  endif()
  laghu_collect_static_analysis_targets(analysis_targets)
  if(analysis_targets STREQUAL "")
    message(FATAL_ERROR "Laghu static analysis failed: first_party_targets_missing")
  endif()
  string(JOIN "," analysis_targets_csv ${analysis_targets})
  add_custom_target(laghu_static_analysis
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE=${CMAKE_SOURCE_DIR}"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
      "-DCXX=${CMAKE_CXX_COMPILER}"
      "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
      "-DCLANG_TIDY=${LAGHU_CLANG_TIDY_EXECUTABLE}"
      "-DSCAN_BUILD=${LAGHU_SCAN_BUILD_EXECUTABLE}"
      "-DCLANG_ANALYZER=${LAGHU_CLANG_ANALYZER_EXECUTABLE}"
      "-DCPPCHECK=${LAGHU_CPPCHECK_EXECUTABLE}"
      "-DCLANG_TIDY_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/clang-tidy-19.yaml"
      "-DCLANG_ANALYZER_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/clang-analyzer-19.txt"
      "-DCPPCHECK_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/cppcheck-2.13.txt"
      "-DANALYSIS_TARGETS=${analysis_targets_csv}"
      -P "${CMAKE_SOURCE_DIR}/cmake/RunStaticAnalysis.cmake"
    DEPENDS ${analysis_targets}
    USES_TERMINAL
    COMMENT "Analyzing Laghu-owned C++ sources")
endfunction()

function(laghu_add_static_analysis_validation_tests)
  if(NOT LAGHU_STATIC_ANALYSIS)
    return()
  endif()
  foreach(tool IN ITEMS clang_tidy clang_analyzer cppcheck)
    add_test(NAME "laghu.static_analysis.negative.${tool}"
      COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=${tool}"
        "-DCLANG_TIDY=${LAGHU_CLANG_TIDY_EXECUTABLE}"
        "-DSCAN_BUILD=${LAGHU_SCAN_BUILD_EXECUTABLE}"
        "-DCLANG_ANALYZER=${LAGHU_CLANG_ANALYZER_EXECUTABLE}"
        "-DCPPCHECK=${LAGHU_CPPCHECK_EXECUTABLE}"
        "-DCLANG_TIDY_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/clang-tidy-19.yaml"
        "-DCLANG_ANALYZER_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/clang-analyzer-19.txt"
        "-DCPPCHECK_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/cppcheck-2.13.txt"
        "-DSOURCE=${CMAKE_SOURCE_DIR}/tests/static-analysis/negative/${tool}.cpp"
        "-DWORK_DIRECTORY=${CMAKE_BINARY_DIR}/tests/static-analysis-negative-${tool}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectStaticAnalysisFailure.cmake")
  endforeach()
  add_test(NAME laghu.static_analysis.ownership.third_party_boundary
    COMMAND "${CMAKE_COMMAND}"
      "-DCLANG_TIDY=${LAGHU_CLANG_TIDY_EXECUTABLE}"
      "-DSCAN_BUILD=${LAGHU_SCAN_BUILD_EXECUTABLE}"
      "-DCLANG_ANALYZER=${LAGHU_CLANG_ANALYZER_EXECUTABLE}"
      "-DCPPCHECK=${LAGHU_CPPCHECK_EXECUTABLE}"
      "-DCLANG_TIDY_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/clang-tidy-19.yaml"
      "-DCPPCHECK_CONFIG=${CMAKE_SOURCE_DIR}/cmake/static-analysis/cppcheck-2.13.txt"
      "-DLAGHU_SOURCE=${CMAKE_SOURCE_DIR}"
      "-DWORK_DIRECTORY=${CMAKE_BINARY_DIR}/tests/static-analysis-ownership"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectStaticAnalysisOwnership.cmake")
endfunction()
