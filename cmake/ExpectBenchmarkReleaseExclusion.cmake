# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED ARCHIVE OR NOT DEFINED EXECUTABLE OR NOT DEFINED NM OR
    NOT DEFINED STAGE_DIRECTORY)
  message(FATAL_ERROR "Laghu benchmark exclusion expectation requires BUILD_DIRECTORY ARCHIVE EXECUTABLE NM and STAGE_DIRECTORY")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}" --target laghu
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_diagnostics)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Laghu benchmark exclusion expectation failed: build=${build_output}${build_diagnostics}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGE_DIRECTORY}"
    "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_diagnostics)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Laghu benchmark exclusion expectation failed: install=${install_output}${install_diagnostics}")
endif()

foreach(artifact IN ITEMS "${ARCHIVE}" "${EXECUTABLE}")
  execute_process(COMMAND "${NM}" -a "${artifact}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE diagnostics)
  if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "Laghu benchmark exclusion expectation failed: nm=${artifact}; diagnostics=${diagnostics}")
  endif()
  if(symbols MATCHES "laghu.*benchmark|benchmark_core_foundation")
    message(FATAL_ERROR "Laghu benchmark exclusion expectation failed: instrumentation_leaked artifact=${artifact}")
  endif()
endforeach()
