# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED MARKER_ARTIFACT OR NOT DEFINED MARKER OR
    NOT DEFINED MARKER_PREFIX OR NOT DEFINED MARKER_TARGET OR NOT DEFINED NM)
  message(FATAL_ERROR "Laghu sanitizer marker rejection requires BUILD_DIRECTORY MARKER_ARTIFACT MARKER MARKER_PREFIX MARKER_TARGET and NM")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIRECTORY=${BUILD_DIRECTORY}"
    "-DARCHIVE=${MARKER_ARTIFACT}"
    "-DEXECUTABLE=${MARKER_ARTIFACT}"
    "-DNM=${NM}"
    "-DMARKER_ARTIFACT=${MARKER_ARTIFACT}"
    "-DMARKER=${MARKER}"
    "-DMARKER_PREFIX=${MARKER_PREFIX}"
    "-DMARKER_TARGET=${MARKER_TARGET}"
    -P "${CMAKE_CURRENT_LIST_DIR}/ExpectSanitizerReleaseExclusion.cmake"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu sanitizer marker rejection failed: marker=${MARKER}; rejection=missing")
endif()
set(combined "${output}${diagnostics}")
if(NOT combined MATCHES "test_symbol=present")
  message(FATAL_ERROR "Laghu sanitizer marker rejection failed: marker=${MARKER}; rejection=unrelated")
endif()
