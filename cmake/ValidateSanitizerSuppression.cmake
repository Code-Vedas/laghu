# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MANIFEST OR NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "Laghu sanitizer suppression validation requires MANIFEST and SOURCE_ROOT")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/LaghuSanitizers.cmake")
laghu_validate_sanitizer_suppressions("${MANIFEST}" "${SOURCE_ROOT}")
