# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED NM OR NOT DEFINED ZLIB_ARCHIVE OR NOT DEFINED BROTLI_ARCHIVE OR
    NOT DEFINED ZSTD_ARCHIVE)
  message(FATAL_ERROR "Laghu codec isolation expectation requires NM and all codec archives")
endif()

foreach(codec IN ITEMS zlib brotli zstd)
  string(TOUPPER "${codec}" upper)
  execute_process(COMMAND "${NM}" "${${upper}_ARCHIVE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "Laghu codec isolation expectation failed: codec=${codec} diagnostics=${diagnostics}")
  endif()
  foreach(other IN ITEMS zlib brotli zstd)
    if(NOT other STREQUAL codec AND symbols MATCHES "create_${other}.*_codec")
      message(FATAL_ERROR
        "Laghu codec isolation expectation failed: codec=${codec} leaked=${other}")
    endif()
  endforeach()
endforeach()
