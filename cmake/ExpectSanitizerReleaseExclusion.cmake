# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED ARCHIVE OR NOT DEFINED EXECUTABLE OR NOT DEFINED NM)
  message(FATAL_ERROR "Laghu sanitizer release exclusion requires BUILD_DIRECTORY ARCHIVE EXECUTABLE and NM")
endif()

if(NOT EXISTS "${ARCHIVE}" OR NOT EXISTS "${EXECUTABLE}")
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}" --target laghu_core laghu
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_diagnostics)
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Laghu sanitizer release exclusion failed: build=${build_output}${build_diagnostics}")
  endif()
endif()

foreach(artifact IN ITEMS "${ARCHIVE}" "${EXECUTABLE}")
  execute_process(COMMAND "${NM}" -g "${artifact}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Laghu sanitizer release exclusion failed: artifact=${artifact}; nm=${diagnostics}")
  endif()
  if(symbols MATCHES "laghu_sanitizer_")
    message(FATAL_ERROR "Laghu sanitizer release exclusion failed: artifact=${artifact}; test_symbol=present")
  endif()
endforeach()
