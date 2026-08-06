# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(LAGHU_SOURCE_MANIFEST "${CMAKE_SOURCE_DIR}/cmake/laghu-sources.list")

function(laghu_manifest_sources group output)
  file(STRINGS "${LAGHU_SOURCE_MANIFEST}" entries)
  set(sources)
  foreach(entry IN LISTS entries)
    if(entry MATCHES "^${group}\\|[^|]+\\|(.+)$")
      list(APPEND sources "${CMAKE_SOURCE_DIR}/${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(NOT sources)
    message(FATAL_ERROR "No sources registered for Laghu group ${group}")
  endif()
  set(${output} "${sources}" PARENT_SCOPE)
endfunction()

function(laghu_manifest_source group logical_name output)
  file(STRINGS "${LAGHU_SOURCE_MANIFEST}" entries)
  set(source)
  foreach(entry IN LISTS entries)
    if(entry MATCHES "^${group}\\|${logical_name}\\|(.+)$")
      if(source)
        message(FATAL_ERROR "Multiple ${group} sources registered as ${logical_name}")
      endif()
      set(source "${CMAKE_SOURCE_DIR}/${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(NOT source)
    message(FATAL_ERROR "No ${group} source registered as ${logical_name}")
  endif()
  set(${output} "${source}" PARENT_SCOPE)
endfunction()
