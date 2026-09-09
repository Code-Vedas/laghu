# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_DEPENDENCY_MODULE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")

# This registry owns both version gates and dependency acquisition. It keeps
# the default core-only configuration dependency-free: only dependencies of an
# effective external feature are acquired.
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

set(LAGHU_DEPENDENCY_SOURCES VENDORED SYSTEM)
set(LAGHU_DEPENDENCY_LINK_MODES STATIC DYNAMIC)
set(LAGHU_TLS_PROVIDERS OPENSSL LIBRESSL)

set(LAGHU_DEPENDENCY_SOURCE VENDORED CACHE STRING "Laghu external dependency source")
set_property(CACHE LAGHU_DEPENDENCY_SOURCE PROPERTY STRINGS ${LAGHU_DEPENDENCY_SOURCES})
set(LAGHU_DEPENDENCY_LINK_MODE STATIC CACHE STRING "Laghu external dependency link mode")
set_property(CACHE LAGHU_DEPENDENCY_LINK_MODE PROPERTY STRINGS ${LAGHU_DEPENDENCY_LINK_MODES})
set(LAGHU_TLS_PROVIDER OPENSSL CACHE STRING "Laghu TLS dependency provider")
set_property(CACHE LAGHU_TLS_PROVIDER PROPERTY STRINGS ${LAGHU_TLS_PROVIDERS})

function(laghu_declare_dependency id)
  cmake_parse_arguments(PARSE_ARGV 1 dependency
    ""
    "VENDORED_VERSION;SYSTEM_FLOOR;ARCHIVE_URL;ARCHIVE_SHA256;PROBE_SOURCE"
    "FEATURES;PKG_CONFIG_NAMES;CMAKE_TARGETS;CMAKE_SOURCE_SUBDIR")
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
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_CMAKE_TARGETS_${id}" "${dependency_CMAKE_TARGETS}")
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_CMAKE_SOURCE_SUBDIR_${id}" "${dependency_CMAKE_SOURCE_SUBDIR}")
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
  # Archive URLs are official upstream project endpoints. SHA-256 pins are
  # calculated from those exact archives and are consumed by the vendored
  # FetchContent or ExternalProject acquisition paths below.
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
    PROBE_SOURCE tests/dependencies/probes/yyjson.cpp FEATURES structured_data PKG_CONFIG_NAMES yyjson
    CMAKE_TARGETS yyjson)
  laghu_declare_dependency(nghttp2
    VENDORED_VERSION 1.70.0 SYSTEM_FLOOR 1.50.0
    ARCHIVE_URL https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz
    ARCHIVE_SHA256 e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2
    PROBE_SOURCE tests/dependencies/probes/nghttp2.cpp FEATURES http2 PKG_CONFIG_NAMES libnghttp2
    CMAKE_TARGETS nghttp2_static nghttp2_shared nghttp2)
  laghu_declare_dependency(ngtcp2
    VENDORED_VERSION 1.25.0 SYSTEM_FLOOR 1.25.0
    ARCHIVE_URL https://github.com/ngtcp2/ngtcp2/releases/download/v1.25.0/ngtcp2-1.25.0.tar.xz
    ARCHIVE_SHA256 2a34d2484ba17847a5d11965704e9dd0fac4c6d8efc75ffe1ec7de66d8c6b6fb
    PROBE_SOURCE tests/dependencies/probes/ngtcp2.cpp FEATURES http3 PKG_CONFIG_NAMES libngtcp2
    CMAKE_TARGETS ngtcp2 ngtcp2_static ngtcp2_shared)
  laghu_declare_dependency(nghttp3
    VENDORED_VERSION 1.18.0 SYSTEM_FLOOR 1.18.0
    ARCHIVE_URL https://github.com/ngtcp2/nghttp3/releases/download/v1.18.0/nghttp3-1.18.0.tar.xz
    ARCHIVE_SHA256 aad782c23d3f01bd4bb52c8bac7a553b631ef8115fd1612703df6183449fef19
    PROBE_SOURCE tests/dependencies/probes/nghttp3.cpp FEATURES http3 PKG_CONFIG_NAMES libnghttp3
    CMAKE_TARGETS nghttp3 nghttp3_static nghttp3_shared)
  laghu_declare_dependency(c_ares
    VENDORED_VERSION 1.34.8 SYSTEM_FLOOR 1.20.0
    ARCHIVE_URL https://github.com/c-ares/c-ares/releases/download/v1.34.8/c-ares-1.34.8.tar.gz
    ARCHIVE_SHA256 c222b6d681096f9444d2c4863d2c1174019e27cacca0a4a5c114d36dd7d7bf78
    PROBE_SOURCE tests/dependencies/probes/c_ares.cpp FEATURES async_dns PKG_CONFIG_NAMES libcares cares
    CMAKE_TARGETS c-ares::cares_static c-ares::cares cares_static cares)
  laghu_declare_dependency(pcre2_8bit
    VENDORED_VERSION 10.48 SYSTEM_FLOOR 10.40
    ARCHIVE_URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.bz2
    ARCHIVE_SHA256 b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed
    PROBE_SOURCE tests/dependencies/probes/pcre2_8bit.cpp FEATURES regex PKG_CONFIG_NAMES libpcre2-8
    CMAKE_TARGETS pcre2-8 pcre2-8-static)
  laghu_declare_dependency(zlib_ng
    VENDORED_VERSION 2.3.3 SYSTEM_FLOOR 2.1.2
    ARCHIVE_URL https://github.com/zlib-ng/zlib-ng/archive/refs/tags/2.3.3.tar.gz
    ARCHIVE_SHA256 f9c65aa9c852eb8255b636fd9f07ce1c406f061ec19a2e7d508b318ca0c907d1
    PROBE_SOURCE tests/dependencies/probes/zlib_ng.cpp FEATURES compression_zlib PKG_CONFIG_NAMES zlib-ng
    CMAKE_TARGETS zlibstatic zlib)
  laghu_declare_dependency(brotli
    VENDORED_VERSION 1.2.0 SYSTEM_FLOOR 1.0.9
    ARCHIVE_URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
    ARCHIVE_SHA256 816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
    PROBE_SOURCE tests/dependencies/probes/brotli.cpp FEATURES compression_brotli PKG_CONFIG_NAMES libbrotlienc
    CMAKE_TARGETS brotlienc brotlienc-static)
  laghu_declare_dependency(zstd
    VENDORED_VERSION 1.5.7 SYSTEM_FLOOR 1.5.0
    ARCHIVE_URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    ARCHIVE_SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    PROBE_SOURCE tests/dependencies/probes/zstd.cpp FEATURES compression_zstd PKG_CONFIG_NAMES libzstd
    CMAKE_TARGETS libzstd_static libzstd_shared
    CMAKE_SOURCE_SUBDIR build/cmake)
  laghu_declare_dependency(libmaxminddb
    VENDORED_VERSION 1.14.0 SYSTEM_FLOOR 1.8.0
    ARCHIVE_URL https://github.com/maxmind/libmaxminddb/releases/download/1.14.0/libmaxminddb-1.14.0.tar.gz
    ARCHIVE_SHA256 65ff92382c71ef6634b8c13e278651a2efa68f1de28ef3c31fc32369fa0bb3e3
    PROBE_SOURCE tests/dependencies/probes/libmaxminddb.cpp FEATURES geoip PKG_CONFIG_NAMES libmaxminddb
    CMAKE_TARGETS maxminddb)
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
  cmake_parse_arguments(PARSE_ARGV 1 system "" "TARGET;LINK_MODE" "")
  if(system_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=invalid_system_request")
  endif()
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
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}" "${${found_prefix}_VERSION}")
  if(system_LINK_MODE STREQUAL "STATIC")
    set(system_libraries ${${found_prefix}_STATIC_LIBRARIES})
    set(system_library_directories ${${found_prefix}_STATIC_LIBRARY_DIRS})
    if(NOT system_libraries)
      message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=static_artifact_missing")
    endif()
    laghu_prove_system_static_artifacts("${id}" "${system_libraries}" "${system_library_directories}" system_libraries)
  else()
    set(system_libraries ${${found_prefix}_LINK_LIBRARIES})
  endif()
  laghu_add_dependency_symbol_probe(
    ID "${id}"
    TARGET "laghu_dependency_probe_${id}"
    INCLUDE_DIRECTORIES ${${found_prefix}_INCLUDE_DIRS}
    LIBRARIES ${system_libraries})
  if(NOT system_TARGET STREQUAL "")
    if(TARGET "${system_TARGET}")
      message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=private_target_exists")
    endif()
    add_library("${system_TARGET}" INTERFACE)
    target_include_directories("${system_TARGET}" SYSTEM INTERFACE ${${found_prefix}_INCLUDE_DIRS})
    target_link_libraries("${system_TARGET}" INTERFACE ${system_libraries})
    set_property(TARGET "${system_TARGET}" PROPERTY LAGHU_DEPENDENCY_ID "${id}")
    set_property(TARGET "${system_TARGET}" PROPERTY LAGHU_DEPENDENCY_SOURCE SYSTEM)
    set_property(TARGET "${system_TARGET}" PROPERTY LAGHU_DEPENDENCY_LINK_MODE "${system_LINK_MODE}")
  endif()
endfunction()

function(laghu_prove_static_artifact id artifact)
  if(NOT EXISTS "${artifact}")
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=static_artifact_missing")
  endif()
  if(NOT artifact MATCHES "\\.a$")
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=static_artifact_required artifact=${artifact}")
  endif()
  execute_process(
    COMMAND "${CMAKE_AR}" -t "${artifact}"
    RESULT_VARIABLE archive_result
    OUTPUT_QUIET
    ERROR_VARIABLE archive_diagnostics)
  if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=static_artifact_invalid")
  endif()
endfunction()

function(laghu_prove_system_static_artifacts id libraries library_directories output)
  set(original_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(resolved_libraries)
  foreach(library IN LISTS libraries)
    if(library MATCHES "^-l(.+)$")
      set(library_name "${CMAKE_MATCH_1}")
    elseif(IS_ABSOLUTE "${library}")
      laghu_prove_static_artifact("${id}" "${library}")
      list(APPEND resolved_libraries "${library}")
      continue()
    else()
      set(library_name "${library}")
    endif()
    string(MAKE_C_IDENTIFIER "${id}_${library_name}" library_variable)
    unset("LAGHU_STATIC_${library_variable}" CACHE)
    find_library("LAGHU_STATIC_${library_variable}"
      NAMES "${library_name}"
      PATHS ${library_directories})
    if(NOT LAGHU_STATIC_${library_variable})
      message(FATAL_ERROR
        "Laghu dependency mode failed: dependency=${id} rule=static_artifact_missing library=${library_name}")
    endif()
    laghu_prove_static_artifact("${id}" "${LAGHU_STATIC_${library_variable}}")
    list(APPEND resolved_libraries "${LAGHU_STATIC_${library_variable}}")
  endforeach()
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${original_suffixes}")
  set(${output} "${resolved_libraries}" PARENT_SCOPE)
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

function(laghu_dependency_mode_fail rule)
  message(FATAL_ERROR "Laghu dependency mode failed: rule=${rule}")
endfunction()

function(laghu_validate_dependency_modes)
  list(FIND LAGHU_DEPENDENCY_SOURCES "${LAGHU_DEPENDENCY_SOURCE}" source_index)
  if(source_index EQUAL -1)
    laghu_dependency_mode_fail("source_invalid source=${LAGHU_DEPENDENCY_SOURCE}")
  endif()
  list(FIND LAGHU_DEPENDENCY_LINK_MODES "${LAGHU_DEPENDENCY_LINK_MODE}" link_index)
  if(link_index EQUAL -1)
    laghu_dependency_mode_fail("link_mode_invalid link_mode=${LAGHU_DEPENDENCY_LINK_MODE}")
  endif()
  list(FIND LAGHU_TLS_PROVIDERS "${LAGHU_TLS_PROVIDER}" provider_index)
  if(provider_index EQUAL -1)
    laghu_dependency_mode_fail("tls_provider_invalid tls_provider=${LAGHU_TLS_PROVIDER}")
  endif()
endfunction()

function(laghu_select_active_dependencies output)
  cmake_parse_arguments(PARSE_ARGV 1 selection "" "TLS_PROVIDER" "FEATURES")
  if(selection_UNPARSED_ARGUMENTS OR selection_TLS_PROVIDER STREQUAL "")
    laghu_dependency_mode_fail("active_selection_invalid")
  endif()
  string(TOLOWER "${selection_TLS_PROVIDER}" tls_dependency)
  if(NOT tls_dependency STREQUAL openssl AND NOT tls_dependency STREQUAL libressl)
    laghu_dependency_mode_fail("tls_provider_invalid tls_provider=${selection_TLS_PROVIDER}")
  endif()
  set(active)
  foreach(feature IN LISTS selection_FEATURES)
    if(feature STREQUAL tls)
      list(APPEND active "${tls_dependency}")
    endif()
    foreach(id IN LISTS LAGHU_DEPENDENCY_IDS)
      if(id STREQUAL openssl OR id STREQUAL libressl)
        continue()
      endif()
      laghu_dependency_property("${id}" FEATURES dependency_features)
      list(FIND dependency_features "${feature}" feature_index)
      if(NOT feature_index EQUAL -1)
        list(APPEND active "${id}")
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES active)
  list(FIND active openssl openssl_index)
  list(FIND active libressl libressl_index)
  if(NOT openssl_index EQUAL -1 AND NOT libressl_index EQUAL -1)
    laghu_dependency_mode_fail("tls_provider_mixed")
  endif()
  set(${output} "${active}" PARENT_SCOPE)
endfunction()

function(laghu_dependency_private_target_name id output)
  laghu_dependency_require_known("${id}" unknown_dependency)
  set(${output} "laghu_dependency_${id}" PARENT_SCOPE)
endfunction()

function(laghu_add_static_artifact_proof id target output)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=target_missing")
  endif()
  get_target_property(target_type "${target}" TYPE)
  if(NOT target_type STREQUAL STATIC_LIBRARY)
    message(FATAL_ERROR "Laghu dependency mode failed: dependency=${id} rule=static_target_required")
  endif()
  string(MAKE_C_IDENTIFIER "${id}_${target}" proof_id)
  set(proof_target "laghu_dependency_static_proof_${proof_id}")
  if(NOT TARGET "${proof_target}")
    add_custom_target("${proof_target}"
      COMMAND "${CMAKE_COMMAND}"
        "-DARTIFACT=$<TARGET_FILE:${target}>"
        "-DDEPENDENCY=${id}"
        "-DAR=${CMAKE_AR}"
        -P "${LAGHU_DEPENDENCY_MODULE_DIRECTORY}/VerifyStaticArtifact.cmake"
      DEPENDS "${target}"
      VERBATIM)
  endif()
  set(LAGHU_DEPENDENCY_STATIC_PROOF_TARGETS
    "${LAGHU_DEPENDENCY_STATIC_PROOF_TARGETS};${proof_target}"
    CACHE INTERNAL "Laghu static dependency proof targets")
  set(${output} "${proof_target}" PARENT_SCOPE)
endfunction()

function(laghu_find_vendored_cmake_target id output)
  laghu_dependency_property("${id}" CMAKE_TARGETS candidates)
  foreach(candidate IN LISTS candidates)
    if(TARGET "${candidate}")
      get_target_property(alias_target "${candidate}" ALIASED_TARGET)
      if(alias_target)
        set(candidate_target "${alias_target}")
      else()
        set(candidate_target "${candidate}")
      endif()
      get_target_property(candidate_type "${candidate_target}" TYPE)
      if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC AND candidate_type STREQUAL STATIC_LIBRARY)
        set(${output} "${candidate_target}" PARENT_SCOPE)
        return()
      endif()
      if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL DYNAMIC AND candidate_type STREQUAL SHARED_LIBRARY)
        set(${output} "${candidate_target}" PARENT_SCOPE)
        return()
      endif()
    endif()
  endforeach()
  message(FATAL_ERROR
    "Laghu dependency mode failed: dependency=${id} rule=vendored_target_for_link_mode_missing link_mode=${LAGHU_DEPENDENCY_LINK_MODE}")
endfunction()

function(laghu_acquire_vendored_cmake_dependency id private_target)
  include(FetchContent)
  laghu_dependency_property("${id}" ARCHIVE_URL archive_url)
  laghu_dependency_property("${id}" ARCHIVE_SHA256 archive_sha256)
  laghu_dependency_property("${id}" CMAKE_SOURCE_SUBDIR cmake_source_subdir)
  set(content_name "laghu_vendor_${id}")
  set(patch_arguments)
  if(id STREQUAL ngtcp2 OR id STREQUAL nghttp3)
    list(APPEND patch_arguments
      PATCH_COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=<SOURCE_DIR>"
        "-DPROJECT_ID=${id}"
        -P "${LAGHU_DEPENDENCY_MODULE_DIRECTORY}/PatchVendoredProject.cmake")
  endif()
  set(source_subdir_arguments)
  if(NOT cmake_source_subdir STREQUAL "")
    list(APPEND source_subdir_arguments SOURCE_SUBDIR "${cmake_source_subdir}")
  endif()
  FetchContent_Declare("${content_name}"
    URL "${archive_url}"
    URL_HASH "SHA256=${archive_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
    ${patch_arguments}
    ${source_subdir_arguments})
  set(BUILD_TESTING OFF)
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    set(BUILD_SHARED_LIBS OFF)
  else()
    set(BUILD_SHARED_LIBS ON)
  endif()
  if(id STREQUAL nghttp2)
    if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
      set(BUILD_STATIC_LIBS ON)
    else()
      set(BUILD_STATIC_LIBS OFF)
    endif()
  endif()
  if(id STREQUAL ngtcp2 OR id STREQUAL nghttp3)
    set(ENABLE_LIB_ONLY ON)
    if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
      set(ENABLE_STATIC_LIB ON)
      set(ENABLE_SHARED_LIB OFF)
    else()
      set(ENABLE_STATIC_LIB OFF)
      set(ENABLE_SHARED_LIB ON)
    endif()
  endif()
  if(id STREQUAL ngtcp2)
    # Laghu owns TLS-provider selection.  ngtcp2's optional crypto backend
    # defaults to ON and otherwise discovers an unrelated host OpenSSL during
    # configuration, which can reject a non-QUIC host provider.  Build only
    # its transport library; the selected Laghu TLS provider remains separate.
    set(ENABLE_OPENSSL OFF)
  endif()
  if(id STREQUAL c_ares)
    set(CARES_BUILD_TOOLS OFF CACHE BOOL "Build c-ares tools" FORCE)
    if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
      set(CARES_STATIC ON CACHE BOOL "Build c-ares static library" FORCE)
      set(CARES_SHARED OFF CACHE BOOL "Build c-ares shared library" FORCE)
    else()
      set(CARES_STATIC OFF CACHE BOOL "Build c-ares static library" FORCE)
      set(CARES_SHARED ON CACHE BOOL "Build c-ares shared library" FORCE)
    endif()
  endif()
  FetchContent_MakeAvailable("${content_name}")
  laghu_dependency_property("${id}" VENDORED_VERSION vendored_version)
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}" "${vendored_version}")
  laghu_find_vendored_cmake_target("${id}" vendored_target)
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    laghu_add_static_artifact_proof("${id}" "${vendored_target}" static_proof_target)
  endif()
  add_library("${private_target}" INTERFACE)
  target_link_libraries("${private_target}" INTERFACE "${vendored_target}")
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    add_dependencies("${private_target}" "${static_proof_target}")
  endif()
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_ID "${id}")
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_SOURCE VENDORED)
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_LINK_MODE "${LAGHU_DEPENDENCY_LINK_MODE}")
endfunction()

function(laghu_acquire_vendored_tls_dependency id private_target)
  include(ExternalProject)
  if(CMAKE_SYSTEM_NAME STREQUAL FreeBSD)
    find_program(laghu_make_program NAMES gmake REQUIRED)
  else()
    find_program(laghu_make_program NAMES make REQUIRED)
  endif()
  laghu_dependency_property("${id}" ARCHIVE_URL archive_url)
  laghu_dependency_property("${id}" ARCHIVE_SHA256 archive_sha256)
  set(prefix "${CMAKE_BINARY_DIR}/_deps/${id}")
  set(install_directory "${prefix}/install")
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    set(library_type STATIC)
    set(configure_mode no-shared)
    set(ssl_library "${install_directory}/lib/libssl.a")
    set(crypto_library "${install_directory}/lib/libcrypto.a")
  elseif(CMAKE_SYSTEM_NAME STREQUAL Darwin)
    set(library_type SHARED)
    set(configure_mode shared)
    set(ssl_library "${install_directory}/lib/libssl.dylib")
    set(crypto_library "${install_directory}/lib/libcrypto.dylib")
  else()
    set(library_type SHARED)
    set(configure_mode shared)
    set(ssl_library "${install_directory}/lib/libssl.so")
    set(crypto_library "${install_directory}/lib/libcrypto.so")
  endif()
  if(id STREQUAL openssl)
    if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL Linux AND
        CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64)
      set(configure_environment
        "${CMAKE_COMMAND}" -E env
        "CC=${CMAKE_C_COMPILER}"
        "AR=${CMAKE_AR}"
        "RANLIB=${CMAKE_RANLIB}")
      set(openssl_target linux-aarch64)
    else()
      set(configure_environment)
      set(openssl_target)
    endif()
    set(configure_command ${configure_environment} "<SOURCE_DIR>/Configure"
      ${openssl_target} "${configure_mode}" no-apps no-tests
      "--prefix=${install_directory}" "--libdir=lib")
    set(install_command "${laghu_make_program}" install_sw)
  else()
    if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
      set(configure_command "<SOURCE_DIR>/configure" "--prefix=${install_directory}" "--disable-shared")
    else()
      set(configure_command "<SOURCE_DIR>/configure" "--prefix=${install_directory}" "--enable-shared")
    endif()
    set(install_command "${laghu_make_program}" install)
  endif()
  ExternalProject_Add("laghu_vendor_${id}"
    PREFIX "${prefix}"
    URL "${archive_url}"
    URL_HASH "SHA256=${archive_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND ${configure_command}
    BUILD_COMMAND "${laghu_make_program}"
    INSTALL_COMMAND ${install_command}
    BUILD_IN_SOURCE TRUE
    EXCLUDE_FROM_ALL TRUE
    BUILD_BYPRODUCTS "${ssl_library}" "${crypto_library}")
  laghu_dependency_property("${id}" VENDORED_VERSION vendored_version)
  set_property(GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}" "${vendored_version}")
  add_library("${private_target}_ssl" "${library_type}" IMPORTED GLOBAL)
  set_target_properties("${private_target}_ssl" PROPERTIES
    IMPORTED_LOCATION "${ssl_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${install_directory}/include")
  add_dependencies("${private_target}_ssl" "laghu_vendor_${id}")
  add_library("${private_target}_crypto" "${library_type}" IMPORTED GLOBAL)
  set_target_properties("${private_target}_crypto" PROPERTIES
    IMPORTED_LOCATION "${crypto_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${install_directory}/include")
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    find_package(Threads REQUIRED)
    target_link_libraries("${private_target}_crypto" INTERFACE Threads::Threads)
  endif()
  add_dependencies("${private_target}_crypto" "laghu_vendor_${id}")
  add_library("${private_target}" INTERFACE)
  target_link_libraries("${private_target}" INTERFACE
    "${private_target}_ssl" "${private_target}_crypto")
  add_dependencies("${private_target}" "laghu_vendor_${id}")
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_ID "${id}")
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_SOURCE VENDORED)
  set_property(TARGET "${private_target}" PROPERTY LAGHU_DEPENDENCY_LINK_MODE "${LAGHU_DEPENDENCY_LINK_MODE}")
  if(LAGHU_DEPENDENCY_LINK_MODE STREQUAL STATIC)
    laghu_add_static_artifact_proof("${id}" "${private_target}_ssl" ssl_static_proof_target)
    laghu_add_static_artifact_proof("${id}" "${private_target}_crypto" crypto_static_proof_target)
    add_dependencies("${private_target}" "${ssl_static_proof_target}" "${crypto_static_proof_target}")
  endif()
endfunction()

function(laghu_write_dependency_selection_metadata output active_dependencies)
  set(entries)
  foreach(id IN LISTS active_dependencies)
    laghu_dependency_property("${id}" VENDORED_VERSION vendored_version)
    laghu_dependency_property("${id}" ARCHIVE_URL archive_url)
    laghu_dependency_property("${id}" ARCHIVE_SHA256 archive_sha256)
    get_property(selected_version GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}")
    if(selected_version STREQUAL "")
      set(selected_version "${vendored_version}")
    endif()
    list(APPEND entries
      "    {\"id\": \"${id}\", \"source\": \"${LAGHU_DEPENDENCY_SOURCE}\", \"link_mode\": \"${LAGHU_DEPENDENCY_LINK_MODE}\", \"version\": \"${selected_version}\", \"archive_url\": \"${archive_url}\", \"archive_sha256\": \"${archive_sha256}\"}")
  endforeach()
  list(JOIN entries ",\n" rendered_entries)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  if(active_dependencies)
    set(selector_members
"  \"source\": \"${LAGHU_DEPENDENCY_SOURCE}\",\n  \"link_mode\": \"${LAGHU_DEPENDENCY_LINK_MODE}\",\n  \"tls_provider\": \"${LAGHU_TLS_PROVIDER}\",\n")
  else()
    set(selector_members "")
  endif()
  file(WRITE "${output}"
"{\n  \"schema_version\": \"laghu-dependency-selection-v1\",\n${selector_members}  \"active_dependencies\": [\n${rendered_entries}\n  ]\n}\n")
endfunction()

function(laghu_configure_dependency_modes)
  laghu_validate_dependency_modes()
  laghu_select_active_dependencies(active_dependencies
    FEATURES ${LAGHU_EFFECTIVE_FEATURES}
    TLS_PROVIDER "${LAGHU_TLS_PROVIDER}")
  foreach(id IN LISTS active_dependencies)
    laghu_dependency_private_target_name("${id}" private_target)
    if(LAGHU_DEPENDENCY_SOURCE STREQUAL SYSTEM)
      laghu_require_system_dependency("${id}"
        TARGET "${private_target}"
        LINK_MODE "${LAGHU_DEPENDENCY_LINK_MODE}")
    elseif(id STREQUAL openssl OR id STREQUAL libressl)
      laghu_acquire_vendored_tls_dependency("${id}" "${private_target}")
    else()
      laghu_acquire_vendored_cmake_dependency("${id}" "${private_target}")
    endif()
    laghu_dependency_property("${id}" FEATURES dependency_features)
    foreach(feature IN LISTS dependency_features)
      list(FIND LAGHU_EFFECTIVE_FEATURES "${feature}" feature_enabled)
      if(NOT feature_enabled EQUAL -1)
        laghu_feature_target_name("${feature}" feature_target)
        target_link_libraries("${feature_target}" INTERFACE "${private_target}")
      endif()
    endforeach()
  endforeach()
  set(metadata "${CMAKE_BINARY_DIR}/config/laghu-dependency-selection-v1.json")
  laghu_write_dependency_selection_metadata("${metadata}" "${active_dependencies}")
  set(LAGHU_DEPENDENCY_SELECTION_METADATA "${metadata}" CACHE INTERNAL "Laghu dependency selection metadata")
  set(LAGHU_ACTIVE_DEPENDENCIES "${active_dependencies}" CACHE INTERNAL
    "Laghu effective external dependency inventory")
endfunction()
