# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MANIFEST)
  message(FATAL_ERROR "Laghu sanitizer suppression validation requires MANIFEST")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/LaghuSanitizers.cmake")
laghu_validate_sanitizer_suppressions("${MANIFEST}")
