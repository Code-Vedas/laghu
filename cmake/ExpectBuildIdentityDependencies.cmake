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
string(JSON license GET "${dependencies}" 0 license)
string(JSON verification GET "${dependencies}" 0 verification)
string(LENGTH "${sha256}" sha256_length)
if(NOT dependency_count EQUAL 1 OR NOT provider STREQUAL "yyjson" OR NOT source STREQUAL "VENDORED" OR
    NOT version STREQUAL "0.13.0" OR NOT linkage STREQUAL "STATIC" OR
    NOT url STREQUAL "https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz" OR
    NOT sha256_length EQUAL 64 OR NOT sha256 MATCHES "^[0-9a-f]+$" OR
    NOT license STREQUAL "MIT" OR NOT verification STREQUAL "verified-archive" OR
    NOT dependency_names STREQUAL "yyjson")
  message(FATAL_ERROR "Laghu build identity dependency expectation failed: inventory_invalid")
endif()

set(LAGHU_DEPENDENCY_SOURCE SYSTEM)
set(LAGHU_DEPENDENCY_LINK_MODE DYNAMIC)
set(LAGHU_ACTIVE_DEPENDENCIES protobuf_c)
set_property(GLOBAL PROPERTY LAGHU_DEPENDENCY_SELECTED_VERSION_protobuf_c 1.4.1)
laghu_build_identity_dependencies(dependencies dependency_names)
string(JSON dependency_count LENGTH "${dependencies}")
foreach(index RANGE 0 1)
  string(JSON provider GET "${dependencies}" ${index} provider)
  foreach(field IN ITEMS source version linkage url sha256 verification)
    string(JSON ${provider}_${field} GET "${dependencies}" ${index} ${field})
  endforeach()
endforeach()
if(NOT dependency_count EQUAL 2 OR
    NOT dependency_names STREQUAL "protobuf_c;protobuf_c_host" OR
    NOT protobuf_c_source STREQUAL SYSTEM OR
    NOT protobuf_c_version STREQUAL 1.4.1 OR
    NOT protobuf_c_linkage STREQUAL DYNAMIC OR
    NOT protobuf_c_url STREQUAL "" OR NOT protobuf_c_sha256 STREQUAL "" OR
    NOT protobuf_c_verification STREQUAL system-package-unverified OR
    NOT protobuf_c_host_source STREQUAL VENDORED OR
    NOT protobuf_c_host_version STREQUAL 1.5.2 OR
    NOT protobuf_c_host_linkage STREQUAL HOST_TOOL OR
    NOT protobuf_c_host_url STREQUAL
      "https://github.com/protobuf-c/protobuf-c/releases/download/v1.5.2/protobuf-c-1.5.2.tar.gz" OR
    NOT protobuf_c_host_sha256 STREQUAL
      "e2c86271873a79c92b58fef7ebf8de1aa0df4738347a8bd5d4e65a80a16d0d24" OR
    NOT protobuf_c_host_verification STREQUAL verified-archive)
  message(FATAL_ERROR
    "Laghu build identity dependency expectation failed: host_inventory_invalid")
endif()
