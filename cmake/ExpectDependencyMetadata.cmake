# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED METADATA OR NOT DEFINED LAGHU_SOURCE OR NOT DEFINED LAGHU_BINARY)
  message(FATAL_ERROR "ExpectDependencyMetadata requires METADATA, LAGHU_SOURCE, and LAGHU_BINARY")
endif()
if(NOT EXISTS "${METADATA}")
  message(FATAL_ERROR "Laghu dependency metadata expectation failed: missing_output=${METADATA}")
endif()
file(READ "${METADATA}" metadata)
foreach(requirement IN ITEMS
    "\"schema_version\": \"laghu-dependency-registry-v1\""
    "\"openssl\": {\"vendored_version\": \"3.5.8\", \"system_floor\": \"3.5.0\""
    "\"libressl\": {\"vendored_version\": \"4.3.2\", \"system_floor\": \"3.9.2\""
    "\"yyjson\": {\"vendored_version\": \"0.13.0\", \"system_floor\": \"0.8.0\""
    "\"nghttp2\": {\"vendored_version\": \"1.70.0\", \"system_floor\": \"1.50.0\""
    "\"ngtcp2\": {\"vendored_version\": \"1.25.0\", \"system_floor\": \"1.25.0\""
    "\"nghttp3\": {\"vendored_version\": \"1.18.0\", \"system_floor\": \"1.18.0\""
    "\"c_ares\": {\"vendored_version\": \"1.34.8\", \"system_floor\": \"1.20.0\""
    "\"pcre2_8bit\": {\"vendored_version\": \"10.48\", \"system_floor\": \"10.40\""
    "\"zlib_ng\": {\"vendored_version\": \"2.3.3\", \"system_floor\": \"2.1.2\""
    "\"brotli\": {\"vendored_version\": \"1.2.0\", \"system_floor\": \"1.0.9\""
    "\"zstd\": {\"vendored_version\": \"1.5.7\", \"system_floor\": \"1.5.0\""
    "\"libmaxminddb\": {\"vendored_version\": \"1.14.0\", \"system_floor\": \"1.8.0\""
    "\"incompatible_versions\": []")
  string(FIND "${metadata}" "${requirement}" requirement_offset)
  if(requirement_offset EQUAL -1)
    message(FATAL_ERROR "Laghu dependency metadata expectation failed: missing_requirement=${requirement}")
  endif()
endforeach()
if(metadata MATCHES "${LAGHU_SOURCE}" OR metadata MATCHES "${LAGHU_BINARY}")
  message(FATAL_ERROR "Laghu dependency metadata expectation failed: filesystem_path_leak")
endif()
