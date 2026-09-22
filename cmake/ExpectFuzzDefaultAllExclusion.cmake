# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SOURCE OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED CXX OR NOT DEFINED GENERATOR OR
    NOT DEFINED SCRIPT OR NOT DEFINED CORPUS)
  message(FATAL_ERROR "Laghu fuzz default-all exclusion requires SOURCE BUILD_DIRECTORY CXX GENERATOR SCRIPT and CORPUS")
endif()

file(REMOVE_RECURSE "${BUILD_DIRECTORY}")
set(configure_arguments
  -S "${SOURCE}"
  -B "${BUILD_DIRECTORY}"
  -G "${GENERATOR}"
  "-DCMAKE_CXX_COMPILER=${CXX}"
  -DLAGHU_SANITIZER_PROFILE=ASAN_UBSAN
  -DLAGHU_BUILD_FUZZERS=ON)
if(DEFINED MAKE_PROGRAM AND NOT MAKE_PROGRAM STREQUAL "")
  list(APPEND configure_arguments "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_arguments}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_diagnostics)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz default-all exclusion failed: configure=${configure_output}${configure_diagnostics}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}" --target all
  RESULT_VARIABLE all_result
  OUTPUT_VARIABLE all_output
  ERROR_VARIABLE all_diagnostics)
if(NOT all_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz default-all exclusion failed: default_all=${all_output}${all_diagnostics}")
endif()

set(fuzzer "${BUILD_DIRECTORY}/laghu_fuzz_binary_envelope")
if(EXISTS "${fuzzer}")
  message(FATAL_ERROR "Laghu fuzz default-all exclusion failed: fuzzer_built_by_default")
endif()

execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --target binary-envelope
    --corpus "${CORPUS}" --runs 1
  RESULT_VARIABLE wrapper_result
  OUTPUT_VARIABLE wrapper_output
  ERROR_VARIABLE wrapper_diagnostics)
if(NOT wrapper_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz default-all exclusion failed: wrapper=${wrapper_output}${wrapper_diagnostics}")
endif()
if(NOT EXISTS "${fuzzer}")
  message(FATAL_ERROR "Laghu fuzz default-all exclusion failed: wrapper_did_not_build_fuzzer")
endif()
