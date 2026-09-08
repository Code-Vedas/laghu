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
set(expected_cli "${STAGE_DIRECTORY}/opt/laghu/bin/laghu")
set(expected_manifest "${STAGE_DIRECTORY}/opt/laghu/share/laghu/laghu-build-manifest-v1.json")
if(NOT EXISTS "${expected_archive}" OR NOT EXISTS "${expected_cli}" OR NOT EXISTS "${expected_manifest}")
  message(FATAL_ERROR "Laghu staged install is missing ${expected_archive}")
endif()

file(READ "${BUILD_DIRECTORY}/config/laghu-build-manifest-v1.json" build_manifest)
file(READ "${expected_manifest}" installed_manifest)
if(NOT build_manifest STREQUAL installed_manifest)
  message(FATAL_ERROR "Laghu staged install manifest differs from the canonical build manifest")
endif()

file(GLOB_RECURSE installed_paths LIST_DIRECTORIES FALSE "${STAGE_DIRECTORY}/*")
list(LENGTH installed_paths installed_count)
list(SORT installed_paths)
set(expected_paths "${expected_cli};${expected_archive};${expected_manifest}")
list(SORT expected_paths)
if(NOT installed_count EQUAL 3 OR NOT installed_paths STREQUAL expected_paths)
  message(FATAL_ERROR "Laghu staged install must contain only laghu, liblaghu_core.a, and its build manifest")
endif()
