# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED ARCHIVE OR NOT DEFINED EXECUTABLE OR NOT DEFINED NM)
  message(FATAL_ERROR "Laghu sanitizer release exclusion requires ARCHIVE EXECUTABLE and NM")
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
