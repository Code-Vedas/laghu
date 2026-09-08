# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED ARTIFACT OR NOT DEFINED FIRST OR NOT DEFINED SECOND)
  message(FATAL_ERROR "Laghu reproducibility comparison requires ARTIFACT, FIRST, and SECOND")
endif()
foreach(path IN ITEMS "${FIRST}" "${SECOND}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Laghu reproducibility failed: artifact=${ARTIFACT} missing_path=${path}")
  endif()
endforeach()

file(SHA256 "${FIRST}" first_sha256)
file(SHA256 "${SECOND}" second_sha256)
if(NOT first_sha256 STREQUAL second_sha256)
  message(FATAL_ERROR
    "Laghu reproducibility failed: artifact=${ARTIFACT} path_a=${FIRST} sha256_a=${first_sha256} path_b=${SECOND} sha256_b=${second_sha256}")
endif()
