# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED STAGE_DIRECTORY)
  message(FATAL_ERROR "Laghu install-layout test requires build and stage directories")
endif()

file(REMOVE_RECURSE "${STAGE_DIRECTORY}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGE_DIRECTORY}"
    "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}" --prefix /opt/laghu
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "Laghu staged install failed: result=${install_result}\n${install_output}${install_error}")
endif()

set(expected_archive "${STAGE_DIRECTORY}/opt/laghu/lib/laghu/liblaghu_core.a")
if(NOT EXISTS "${expected_archive}")
  message(FATAL_ERROR "Laghu staged install is missing ${expected_archive}")
endif()

file(GLOB_RECURSE installed_paths LIST_DIRECTORIES FALSE "${STAGE_DIRECTORY}/*")
list(LENGTH installed_paths installed_count)
if(NOT installed_count EQUAL 1 OR NOT installed_paths STREQUAL "${expected_archive}")
  message(FATAL_ERROR "Laghu staged install must contain only liblaghu_core.a")
endif()
