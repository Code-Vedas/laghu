# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED METADATA OR NOT DEFINED LAGHU_SOURCE OR NOT DEFINED LAGHU_BINARY)
  message(FATAL_ERROR "ExpectFeatureMetadata requires METADATA, LAGHU_SOURCE, and LAGHU_BINARY")
endif()
if(NOT EXISTS "${METADATA}")
  message(FATAL_ERROR "Laghu feature metadata expectation failed: missing_output=${METADATA}")
endif()
file(READ "${METADATA}" metadata)
foreach(feature IN ITEMS core tls http2 http3 structured_data async_dns regex compression_zlib compression_brotli compression_zstd geoip)
  if(NOT metadata MATCHES "\"${feature}\": ")
    message(FATAL_ERROR "Laghu feature metadata expectation failed: missing_feature=${feature}")
  endif()
endforeach()
string(FIND "${metadata}" "\"requested\": [\"core\"]" requested_offset)
if(requested_offset EQUAL -1)
  message(FATAL_ERROR "Laghu feature metadata expectation failed: requested_features_not_core")
endif()
string(FIND "${metadata}" "\"effective\": [\"core\"]" effective_offset)
if(effective_offset EQUAL -1)
  message(FATAL_ERROR "Laghu feature metadata expectation failed: effective_features_not_core")
endif()
string(FIND "${metadata}" "\"http3\": {\"required\": false, \"depends\": [\"tls\"]" http3_offset)
if(http3_offset EQUAL -1)
  message(FATAL_ERROR "Laghu feature metadata expectation failed: http3_tls_dependency_missing")
endif()
foreach(requirement IN ITEMS
    "\"tls\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [], \"providers\": [\"OPENSSL\", \"LIBRESSL\"]}"
    "\"http2\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"nghttp2\"]"
    "\"http3\": {\"required\": false, \"depends\": [\"tls\"], \"conflicts\": [], \"external_dependencies\": [\"ngtcp2\", \"nghttp3\"]"
    "\"structured_data\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"yyjson\"]"
    "\"async_dns\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"c-ares\"]"
    "\"regex\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"pcre2_8bit\"]"
    "\"compression_zlib\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"zlib_ng\"]"
    "\"compression_brotli\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"brotli\"]"
    "\"compression_zstd\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"zstd\"]"
    "\"geoip\": {\"required\": false, \"depends\": [], \"conflicts\": [], \"external_dependencies\": [\"libmaxminddb\"]")
  string(FIND "${metadata}" "${requirement}" requirement_offset)
  if(requirement_offset EQUAL -1)
    message(FATAL_ERROR "Laghu feature metadata expectation failed: dependency_mapping_missing")
  endif()
endforeach()
if(metadata MATCHES "${LAGHU_SOURCE}" OR metadata MATCHES "${LAGHU_BINARY}")
  message(FATAL_ERROR "Laghu feature metadata expectation failed: filesystem_path_leak")
endif()
