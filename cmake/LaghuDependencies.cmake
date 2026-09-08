# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

# This registry deliberately contains no acquisition policy.  A later source
# and linkage-mode layer supplies a selected dependency and calls the gate
# functions below.  Keeping the contract here lets minimal builds remain
# dependency-free while ensuring every enabled external feature has one
# version and symbol contract.
set(LAGHU_DEPENDENCY_IDS
  openssl
  libressl
  yyjson
  nghttp2
  ngtcp2
  nghttp3
  c_ares
  pcre2_8bit
  zlib_ng
  brotli
  zstd
  libmaxminddb)

function(laghu_declare_dependency id)
  cmake_parse_arguments(PARSE_ARGV 1 dependency
    ""
    "VENDORED_VERSION;SYSTEM_FLOOR;ARCHIVE_URL;ARCHIVE_SHA256;PROBE_SOURCE"
    "FEATURES;PKG_CONFIG_NAMES")
  list(FIND LAGHU_DEPENDENCY_IDS "${id}" id_index)
  if(id_index EQUAL -1 OR dependency_UNPARSED_ARGUMENTS OR
      dependency_VENDORED_VERSION STREQUAL "" OR dependency_SYSTEM_FLOOR STREQUAL "" OR
      dependency_ARCHIVE_URL STREQUAL "" OR dependency_ARCHIVE_SHA256 STREQUAL "" OR
      dependency_PROBE_SOURCE STREQUAL "")
    message(FATAL_ERROR "Laghu dependency registry failed: dependency=${id} rule=invalid_declaration")
  endif()
  string(LENGTH "${dependency_ARCHIVE_SHA256}" archive_sha256_length)
  if(NOT archive_sha256_length EQUAL 64 OR NOT dependency_ARCHIVE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Laghu dependency registry failed: dependency=${id} rule=invalid_archive_sha256")
  endif()
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_VENDORED_VERSION_${id}" "${dependency_VENDORED_VERSION}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_SYSTEM_FLOOR_${id}" "${dependency_SYSTEM_FLOOR}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_ARCHIVE_URL_${id}" "${dependency_ARCHIVE_URL}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_ARCHIVE_SHA256_${id}" "${dependency_ARCHIVE_SHA256}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_PROBE_SOURCE_${id}" "${dependency_PROBE_SOURCE}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_FEATURES_${id}" "${dependency_FEATURES}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_PKG_CONFIG_NAMES_${id}" "${dependency_PKG_CONFIG_NAMES}")
  # Known-incompatible ranges intentionally remain empty until a primary
  # upstream source establishes one.  This prevents invented exclusions.
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_INCOMPATIBLE_RANGES_${id}" "")
endfunction()

function(laghu_dependency_require_known id rule)
  list(FIND LAGHU_DEPENDENCY_IDS "${id}" id_index)
  if(id_index EQUAL -1)
    message(FATAL_ERROR "Laghu dependency registry failed: dependency=${id} rule=${rule}")
  endif()
endfunction()

function(laghu_dependency_property id name output)
  laghu_dependency_require_known("${id}" unknown_dependency)
  get_property(value GLOBAL PROPERTY "LAGHU_DEPENDENCY_${name}_${id}")
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(laghu_dependency_registry_initialize)
  if(DEFINED LAGHU_SOURCE AND EXISTS "${LAGHU_SOURCE}/tests/dependencies/probes")
    set(dependency_source_root "${LAGHU_SOURCE}")
  else()
    set(dependency_source_root "${CMAKE_SOURCE_DIR}")
  endif()
  set_property(GLOBAL PROPERTY LAGHU_DEPENDENCY_SOURCE_ROOT "${dependency_source_root}")
  # Archive URLs are official upstream project endpoints.  SHA-256 pins are
  # calculated from those exact archives; FetchContent/ExternalProject policy
  # is intentionally deferred to Task #31.
  laghu_declare_dependency(openssl
    VENDORED_VERSION 3.5.8 SYSTEM_FLOOR 3.5.0
    ARCHIVE_URL https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
    ARCHIVE_SHA256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
    PROBE_SOURCE tests/dependencies/probes/openssl.cpp FEATURES tls http3 PKG_CONFIG_NAMES openssl)
  laghu_declare_dependency(libressl
    VENDORED_VERSION 4.3.2 SYSTEM_FLOOR 3.9.2
    ARCHIVE_URL https://cdn.openbsd.org/pub/OpenBSD/LibreSSL/libressl-4.3.2.tar.gz
    ARCHIVE_SHA256 edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5
    PROBE_SOURCE tests/dependencies/probes/libressl.cpp FEATURES tls http3 PKG_CONFIG_NAMES libressl)
  laghu_declare_dependency(yyjson
    VENDORED_VERSION 0.13.0 SYSTEM_FLOOR 0.8.0
    ARCHIVE_URL https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz
    ARCHIVE_SHA256 34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae
    PROBE_SOURCE tests/dependencies/probes/yyjson.cpp FEATURES structured_data PKG_CONFIG_NAMES yyjson)
  laghu_declare_dependency(nghttp2
    VENDORED_VERSION 1.70.0 SYSTEM_FLOOR 1.50.0
    ARCHIVE_URL https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz
    ARCHIVE_SHA256 e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
    PROBE_SOURCE tests/dependencies/probes/nghttp2.cpp FEATURES http2 PKG_CONFIG_NAMES libnghttp2)
  laghu_declare_dependency(ngtcp2
    VENDORED_VERSION 1.25.0 SYSTEM_FLOOR 1.25.0
    ARCHIVE_URL https://github.com/ngtcp2/ngtcp2/releases/download/v1.25.0/ngtcp2-1.25.0.tar.xz
    ARCHIVE_SHA256 2a34d2484ba17847a5d11965704e9dd0fac4c6d8efc75ffe1ec7de66d8c6b6fb
    PROBE_SOURCE tests/dependencies/probes/ngtcp2.cpp FEATURES http3 PKG_CONFIG_NAMES libngtcp2)
  laghu_declare_dependency(nghttp3
    VENDORED_VERSION 1.18.0 SYSTEM_FLOOR 1.18.0
    ARCHIVE_URL https://github.com/ngtcp2/nghttp3/archive/refs/tags/v1.18.0.tar.gz
    ARCHIVE_SHA256 6558c14929a79ced8de1cb8bb4e4b17974e531616501ffb0431135bb495f5be7
    PROBE_SOURCE tests/dependencies/probes/nghttp3.cpp FEATURES http3 PKG_CONFIG_NAMES libnghttp3)
  laghu_declare_dependency(c_ares
    VENDORED_VERSION 1.34.8 SYSTEM_FLOOR 1.20.0
    ARCHIVE_URL https://github.com/c-ares/c-ares/releases/download/v1.34.8/c-ares-1.34.8.tar.gz
    ARCHIVE_SHA256 c222b6d681096f9444d2c4863d2c1174019e27cacca0a4a5c114d36dd7d7bf78
    PROBE_SOURCE tests/dependencies/probes/c_ares.cpp FEATURES async_dns PKG_CONFIG_NAMES libcares cares)
  laghu_declare_dependency(pcre2_8bit
    VENDORED_VERSION 10.48 SYSTEM_FLOOR 10.40
    ARCHIVE_URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.bz2
    ARCHIVE_SHA256 b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed
    PROBE_SOURCE tests/dependencies/probes/pcre2_8bit.cpp FEATURES regex PKG_CONFIG_NAMES libpcre2-8)
  laghu_declare_dependency(zlib_ng
    VENDORED_VERSION 2.3.3 SYSTEM_FLOOR 2.1.2
    ARCHIVE_URL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz
    ARCHIVE_SHA256 f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
    PROBE_SOURCE tests/dependencies/probes/zlib_ng.cpp FEATURES compression_zlib PKG_CONFIG_NAMES zlib-ng)
  laghu_declare_dependency(brotli
    VENDORED_VERSION 1.2.0 SYSTEM_FLOOR 1.0.9
    ARCHIVE_URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
    ARCHIVE_SHA256 816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
    PROBE_SOURCE tests/dependencies/probes/brotli.cpp FEATURES compression_brotli PKG_CONFIG_NAMES libbrotlienc)
  laghu_declare_dependency(zstd
    VENDORED_VERSION 1.5.7 SYSTEM_FLOOR 1.5.0
    ARCHIVE_URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    ARCHIVE_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    PROBE_SOURCE tests/dependencies/probes/zstd.cpp FEATURES compression_zstd PKG_CONFIG_NAMES libzstd)
  laghu_declare_dependency(libmaxminddb
    VENDORED_VERSION 1.14.0 SYSTEM_FLOOR 1.8.0
    ARCHIVE_URL https://github.com/maxmind/libmaxminddb/releases/download/1.14.0/libmaxminddb-1.14.0.tar.gz
    ARCHIVE_SHA256 65ff92382c71ef6634b8c13e278651a2efa68f1de28ef3c31fc32369fa0bb3e3
    PROBE_SOURCE tests/dependencies/probes/libmaxminddb.cpp FEATURES geoip PKG_CONFIG_NAMES libmaxminddb)
endfunction()

function(laghu_validate_dependency_version id version)
  laghu_dependency_require_known("${id}" unknown_dependency)
  laghu_dependency_property("${id}" SYSTEM_FLOOR floor)
  if(version STREQUAL "")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${id} rule=version_missing required=${floor}")
  endif()
  if(version VERSION_LESS floor)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${id} rule=version_too_old found=${version} required=${floor}")
  endif()
endfunction()

function(laghu_add_dependency_symbol_probe)
  cmake_parse_arguments(PARSE_ARGV 0 probe "" "ID;TARGET" "INCLUDE_DIRECTORIES;LIBRARIES")
  if(probe_UNPARSED_ARGUMENTS OR probe_ID STREQUAL "" OR probe_TARGET STREQUAL "")
    message(FATAL_ERROR "Laghu dependency gate failed: rule=invalid_symbol_probe")
  endif()
  laghu_dependency_require_known("${probe_ID}" unknown_dependency)
  if(NOT probe_LIBRARIES)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${probe_ID} rule=library_missing")
  endif()
  laghu_dependency_property("${probe_ID}" PROBE_SOURCE source)
  get_property(source_root GLOBAL PROPERTY LAGHU_DEPENDENCY_SOURCE_ROOT)
  if(NOT EXISTS "${source_root}/${source}")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${probe_ID} rule=probe_source_missing")
  endif()
  add_executable("${probe_TARGET}" EXCLUDE_FROM_ALL "${source_root}/${source}")
  set_property(TARGET "${probe_TARGET}" PROPERTY CXX_STANDARD 23)
  set_property(TARGET "${probe_TARGET}" PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET "${probe_TARGET}" PROPERTY CXX_EXTENSIONS OFF)
  target_include_directories("${probe_TARGET}" SYSTEM PRIVATE ${probe_INCLUDE_DIRECTORIES})
  target_link_libraries("${probe_TARGET}" PRIVATE ${probe_LIBRARIES})
endfunction()

function(laghu_require_system_dependency id)
  laghu_dependency_require_known("${id}" unknown_dependency)
  find_package(PkgConfig QUIET)
  if(NOT PkgConfig_FOUND)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${id} rule=pkg_config_unavailable")
  endif()
  laghu_dependency_property("${id}" PKG_CONFIG_NAMES package_names)
  set(prefix "LAGHU_SYSTEM_${id}")
  set(found_prefix "")
  set(package_index 0)
  foreach(package_name IN LISTS package_names)
    math(EXPR package_index "${package_index} + 1")
    set(candidate_prefix "${prefix}_${package_index}")
    pkg_check_modules(${candidate_prefix} QUIET "${package_name}")
    if(${candidate_prefix}_FOUND)
      set(found_prefix "${candidate_prefix}")
      break()
    endif()
  endforeach()
  if(found_prefix STREQUAL "")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=${id} rule=not_found")
  endif()
  laghu_validate_dependency_version("${id}" "${${found_prefix}_VERSION}")
  laghu_add_dependency_symbol_probe(
    ID "${id}"
    TARGET "laghu_dependency_probe_${id}"
    INCLUDE_DIRECTORIES ${${found_prefix}_INCLUDE_DIRS}
    LIBRARIES ${${found_prefix}_LINK_LIBRARIES})
endfunction()

function(laghu_validate_zlib_ng_configuration)
  cmake_parse_arguments(PARSE_ARGV 0 zlib_ng "" "ZLIB_COMPAT" "")
  if(zlib_ng_UNPARSED_ARGUMENTS OR zlib_ng_ZLIB_COMPAT STREQUAL "" OR
      NOT zlib_ng_ZLIB_COMPAT STREQUAL "OFF")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=zlib_ng rule=zlib_compat_must_be_off")
  endif()
endfunction()

function(laghu_validate_http3_dependency_group)
  cmake_parse_arguments(PARSE_ARGV 0 group "" "TLS_PROVIDER" "DEPENDENCIES;VERSIONS")
  if(group_UNPARSED_ARGUMENTS OR group_TLS_PROVIDER STREQUAL "")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=tls_provider_missing")
  endif()
  string(TOLOWER "${group_TLS_PROVIDER}" provider)
  if(NOT provider STREQUAL "openssl" AND NOT provider STREQUAL "libressl")
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=tls_provider_invalid provider=${group_TLS_PROVIDER}")
  endif()
  set(required "${provider};ngtcp2;nghttp3")
  set(actual ${group_DEPENDENCIES})
  list(LENGTH actual declared_count)
  list(REMOVE_DUPLICATES actual)
  list(LENGTH actual unique_count)
  if(NOT declared_count EQUAL unique_count)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=atomic_group_duplicate")
  endif()
  if(provider STREQUAL "openssl")
    list(FIND actual libressl mixed_provider)
  else()
    list(FIND actual openssl mixed_provider)
  endif()
  if(NOT mixed_provider EQUAL -1)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=tls_provider_mixed")
  endif()
  if(NOT unique_count EQUAL 3)
    message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=atomic_group_incomplete provider=${provider}")
  endif()
  foreach(dependency IN LISTS required)
    list(FIND actual "${dependency}" dependency_index)
    if(dependency_index EQUAL -1)
      message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=atomic_group_incomplete provider=${provider} required=${dependency}")
    endif()
  endforeach()
  set(seen_versions)
  foreach(version_entry IN LISTS group_VERSIONS)
    if(NOT version_entry MATCHES "^[a-z0-9_]+=.+$")
      message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=invalid_group_version")
    endif()
    string(REPLACE "=" ";" version_parts "${version_entry}")
    list(GET version_parts 0 version_dependency)
    list(GET version_parts 1 version_value)
    list(FIND required "${version_dependency}" required_index)
    list(FIND seen_versions "${version_dependency}" duplicate_index)
    if(required_index EQUAL -1 OR NOT duplicate_index EQUAL -1)
      message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=invalid_group_version dependency=${version_dependency}")
    endif()
    list(APPEND seen_versions "${version_dependency}")
    laghu_validate_dependency_version("${version_dependency}" "${version_value}")
  endforeach()
  foreach(dependency IN LISTS required)
    list(FIND seen_versions "${dependency}" version_index)
    if(version_index EQUAL -1)
      message(FATAL_ERROR "Laghu dependency gate failed: dependency=http3 rule=atomic_group_version_missing dependency=${dependency}")
    endif()
  endforeach()
endfunction()

function(laghu_dependency_json_array output)
  if(ARGN)
    list(JOIN ARGN "\", \"" members)
    set(${output} "[\"${members}\"]" PARENT_SCOPE)
  else()
    set(${output} "[]" PARENT_SCOPE)
  endif()
endfunction()

function(laghu_write_dependency_registry_metadata output)
  set(members)
  foreach(id IN LISTS LAGHU_DEPENDENCY_IDS)
    foreach(property IN ITEMS VENDORED_VERSION SYSTEM_FLOOR ARCHIVE_URL ARCHIVE_SHA256 FEATURES INCOMPATIBLE_RANGES)
      laghu_dependency_property("${id}" "${property}" "${property}")
    endforeach()
    laghu_dependency_json_array(features_json ${FEATURES})
    laghu_dependency_json_array(incompatible_json ${INCOMPATIBLE_RANGES})
    list(APPEND members
      "    \"${id}\": {\"vendored_version\": \"${VENDORED_VERSION}\", \"system_floor\": \"${SYSTEM_FLOOR}\", \"archive_url\": \"${ARCHIVE_URL}\", \"archive_sha256\": \"${ARCHIVE_SHA256}\", \"features\": ${features_json}, \"incompatible_versions\": ${incompatible_json}}")
  endforeach()
  list(JOIN members ",\n" rendered_members)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  file(WRITE "${output}"
"{\n  \"schema_version\": \"laghu-dependency-registry-v1\",\n  \"dependencies\": {\n${rendered_members}\n  }\n}\n")
endfunction()

function(laghu_configure_dependency_registry)
  laghu_dependency_registry_initialize()
  set(metadata "${CMAKE_BINARY_DIR}/config/laghu-dependency-registry-v1.json")
  laghu_write_dependency_registry_metadata("${metadata}")
  set(LAGHU_DEPENDENCY_REGISTRY_METADATA "${metadata}" CACHE INTERNAL "Laghu dependency registry metadata")
endfunction()
