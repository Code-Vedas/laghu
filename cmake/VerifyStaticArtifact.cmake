# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED ARTIFACT OR NOT DEFINED DEPENDENCY OR NOT DEFINED AR)
  message(FATAL_ERROR "Laghu dependency mode failed: rule=static_artifact_arguments_missing")
endif()
if(NOT EXISTS "${ARTIFACT}")
  message(FATAL_ERROR
    "Laghu dependency mode failed: dependency=${DEPENDENCY} rule=static_artifact_missing")
endif()
if(NOT ARTIFACT MATCHES "\\.a$")
  message(FATAL_ERROR
    "Laghu dependency mode failed: dependency=${DEPENDENCY} rule=static_artifact_required artifact=${ARTIFACT}")
endif()
execute_process(
  COMMAND "${AR}" -t "${ARTIFACT}"
  RESULT_VARIABLE archive_result
  OUTPUT_QUIET
  ERROR_VARIABLE archive_diagnostics)
if(NOT archive_result EQUAL 0)
  message(FATAL_ERROR
    "Laghu dependency mode failed: dependency=${DEPENDENCY} rule=static_artifact_invalid")
endif()
