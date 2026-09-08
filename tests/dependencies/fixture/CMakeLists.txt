# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.28)
project(laghu_dependency_fixture LANGUAGES CXX)

# Dependency fixtures compile ordinary C++23 sources, never C++ modules. Keep
# this local because nested CMake configurations do not inherit the root setting.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED SCENARIO)
  message(FATAL_ERROR "Laghu dependency fixture requires LAGHU_SOURCE and SCENARIO")
endif()

include("${LAGHU_SOURCE}/cmake/LaghuDependencies.cmake")
laghu_dependency_registry_initialize()

if(SCENARIO STREQUAL "positive_symbol" OR SCENARIO STREQUAL "capability_mismatch")
  add_library(yyjson_fixture STATIC yyjson.cpp)
  target_include_directories(yyjson_fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${SCENARIO}/include")
  laghu_add_dependency_symbol_probe(
    ID yyjson
    TARGET laghu_dependency_fixture_probe
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/${SCENARIO}/include"
    LIBRARIES yyjson_fixture)
elseif(SCENARIO STREQUAL "zlib_compat_enabled")
  laghu_validate_zlib_ng_configuration(ZLIB_COMPAT ON)
elseif(SCENARIO STREQUAL "system_openssl")
  laghu_require_system_dependency(openssl)
elseif(SCENARIO STREQUAL "system_dependency")
  if(NOT DEFINED DEPENDENCY)
    message(FATAL_ERROR "Laghu system dependency fixture requires DEPENDENCY")
  endif()
  laghu_require_system_dependency("${DEPENDENCY}")
elseif(SCENARIO STREQUAL "version_too_old")
  laghu_validate_dependency_version(yyjson 0.7.9)
elseif(SCENARIO STREQUAL "unknown_dependency")
  laghu_validate_dependency_version(unknown 1.0.0)
elseif(SCENARIO STREQUAL "positive_http3")
  laghu_validate_http3_dependency_group(
    TLS_PROVIDER OPENSSL
    DEPENDENCIES openssl ngtcp2 nghttp3
    VERSIONS openssl=3.5.0 ngtcp2=1.25.0 nghttp3=1.18.0)
elseif(SCENARIO STREQUAL "http3_incomplete")
  laghu_validate_http3_dependency_group(
    TLS_PROVIDER OPENSSL
    DEPENDENCIES openssl ngtcp2
    VERSIONS openssl=3.5.0 ngtcp2=1.25.0)
elseif(SCENARIO STREQUAL "http3_mixed_provider")
  laghu_validate_http3_dependency_group(
    TLS_PROVIDER OPENSSL
    DEPENDENCIES openssl libressl ngtcp2 nghttp3
    VERSIONS openssl=3.5.0 libressl=3.9.2 ngtcp2=1.25.0 nghttp3=1.18.0)
elseif(SCENARIO STREQUAL "http3_tls_too_old")
  laghu_validate_http3_dependency_group(
    TLS_PROVIDER OPENSSL
    DEPENDENCIES openssl ngtcp2 nghttp3
    VERSIONS openssl=3.4.9 ngtcp2=1.25.0 nghttp3=1.18.0)
else()
  message(FATAL_ERROR "Laghu dependency fixture requires a known scenario; scenario=${SCENARIO}")
endif()
