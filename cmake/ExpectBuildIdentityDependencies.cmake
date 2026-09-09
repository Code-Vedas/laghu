# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED LAGHU_SOURCE)
  message(FATAL_ERROR "Laghu build identity dependency expectation requires LAGHU_SOURCE")
endif()

include("${LAGHU_SOURCE}/cmake/LaghuDependencies.cmake")
include("${LAGHU_SOURCE}/cmake/LaghuBuildIdentity.cmake")
laghu_dependency_registry_initialize()
set(LAGHU_DEPENDENCY_SOURCE VENDORED)
set(LAGHU_DEPENDENCY_LINK_MODE STATIC)
set(LAGHU_ACTIVE_DEPENDENCIES yyjson)
set_property(GLOBAL PROPERTY LAGHU_DEPENDENCY_SELECTED_VERSION_yyjson 0.13.0)
laghu_build_identity_dependencies(dependencies dependency_names)
string(JSON dependency_count LENGTH "${dependencies}")
string(JSON provider GET "${dependencies}" 0 provider)
string(JSON source GET "${dependencies}" 0 source)
string(JSON version GET "${dependencies}" 0 version)
string(JSON linkage GET "${dependencies}" 0 linkage)
string(JSON url GET "${dependencies}" 0 url)
string(JSON sha256 GET "${dependencies}" 0 sha256)
string(LENGTH "${sha256}" sha256_length)
if(NOT dependency_count EQUAL 1 OR NOT provider STREQUAL "yyjson" OR NOT source STREQUAL "VENDORED" OR
    NOT version STREQUAL "0.13.0" OR NOT linkage STREQUAL "STATIC" OR
    NOT url STREQUAL "https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz" OR
    NOT sha256_length EQUAL 64 OR NOT sha256 MATCHES "^[0-9a-f]+$" OR NOT dependency_names STREQUAL "yyjson")
  message(FATAL_ERROR "Laghu build identity dependency expectation failed: inventory_invalid")
endif()
