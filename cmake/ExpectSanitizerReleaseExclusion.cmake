# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED ARCHIVE OR NOT DEFINED EXECUTABLE OR NOT DEFINED NM OR
    NOT DEFINED MARKER_ARTIFACT OR NOT DEFINED MARKER OR NOT DEFINED MARKER_PREFIX OR NOT DEFINED MARKER_TARGET)
  message(FATAL_ERROR "Laghu sanitizer release exclusion requires BUILD_DIRECTORY ARCHIVE EXECUTABLE NM MARKER_ARTIFACT MARKER MARKER_PREFIX and MARKER_TARGET")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}" --target
    laghu_core laghu "${MARKER_TARGET}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_diagnostics)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Laghu sanitizer release exclusion failed: build=${build_output}${build_diagnostics}")
endif()

execute_process(COMMAND "${NM}" -g "${MARKER_ARTIFACT}"
  RESULT_VARIABLE marker_result
  OUTPUT_VARIABLE marker_symbols
  ERROR_VARIABLE marker_diagnostics)
if(NOT marker_result EQUAL 0)
  message(FATAL_ERROR "Laghu sanitizer release exclusion failed: marker_artifact=${MARKER_ARTIFACT}; nm=${marker_diagnostics}")
endif()
if(NOT marker_symbols MATCHES "${MARKER}")
  message(FATAL_ERROR "Laghu sanitizer release exclusion failed: marker_artifact=${MARKER_ARTIFACT}; test_symbol=missing")
endif()

foreach(artifact IN ITEMS "${ARCHIVE}" "${EXECUTABLE}")
  execute_process(COMMAND "${NM}" -g "${artifact}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Laghu sanitizer release exclusion failed: artifact=${artifact}; nm=${diagnostics}")
  endif()
  if(symbols MATCHES "${MARKER_PREFIX}")
    message(FATAL_ERROR "Laghu sanitizer release exclusion failed: artifact=${artifact}; test_symbol=present")
  endif()
endforeach()
