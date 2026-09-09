# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_BUILD_PROFILES MINIMAL FULL CUSTOM)
set(LAGHU_BUILD_PROFILE MINIMAL CACHE STRING "Laghu build feature profile")
set_property(CACHE LAGHU_BUILD_PROFILE PROPERTY STRINGS ${LAGHU_BUILD_PROFILES})
set(LAGHU_FEATURES "" CACHE STRING "Laghu custom build feature IDs")

function(laghu_build_capability_fail)
  message(FATAL_ERROR "Laghu build capability failed: ${ARGN}")
endfunction()

function(laghu_write_build_variant_metadata output profile requested effective)
  laghu_feature_json_array(requested_json ${requested})
  laghu_feature_json_array(effective_json ${effective})
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  file(WRITE "${output}"
"{\n  \"schema_version\": \"laghu-build-variant-v1\",\n  \"profile\": \"${profile}\",\n  \"requested_features\": ${requested_json},\n  \"effective_features\": ${effective_json}\n}\n")
endfunction()

function(laghu_configure_build_variant)
  cmake_parse_arguments(PARSE_ARGV 0 configuration "" "" "AVAILABLE")
  if(configuration_UNPARSED_ARGUMENTS)
    laghu_build_capability_fail("rule=invalid_configuration")
  endif()

  list(FIND LAGHU_BUILD_PROFILES "${LAGHU_BUILD_PROFILE}" profile_index)
  if(profile_index EQUAL -1)
    laghu_build_capability_fail("profile=${LAGHU_BUILD_PROFILE} rule=profile_invalid")
  endif()

  if(LAGHU_BUILD_PROFILE STREQUAL MINIMAL)
    if(NOT LAGHU_FEATURES STREQUAL "")
      laghu_build_capability_fail(
        "profile=MINIMAL rule=feature_selector_not_allowed")
    endif()
    set(requested core)
  elseif(LAGHU_BUILD_PROFILE STREQUAL FULL)
    if(NOT LAGHU_FEATURES STREQUAL "")
      laghu_build_capability_fail(
        "profile=FULL rule=feature_selector_not_allowed")
    endif()
    set(requested ${LAGHU_FEATURE_IDS})
  else()
    set(requested ${LAGHU_FEATURES})
    if(NOT requested)
      laghu_build_capability_fail("profile=CUSTOM rule=feature_selector_required")
    endif()
  endif()

  if(configuration_AVAILABLE)
    set(available ${configuration_AVAILABLE})
  else()
    set(available ${LAGHU_FEATURE_IDS})
  endif()

  laghu_configure_feature_registry(
    REQUESTED ${requested}
    AVAILABLE ${available}
    DIAGNOSTIC_PREFIX "Laghu build capability failed")
  set(LAGHU_BUILD_REQUESTED_FEATURES "${requested}" CACHE INTERNAL
    "Laghu build profile requested features")
  set(LAGHU_BUILD_EFFECTIVE_FEATURES "${LAGHU_EFFECTIVE_FEATURES}" CACHE INTERNAL
    "Laghu build profile effective features")
  set(metadata "${CMAKE_BINARY_DIR}/config/laghu-build-variant-v1.json")
  laghu_write_build_variant_metadata(
    "${metadata}" "${LAGHU_BUILD_PROFILE}" "${requested}" "${LAGHU_EFFECTIVE_FEATURES}")
  set(LAGHU_BUILD_VARIANT_METADATA "${metadata}" CACHE INTERNAL
    "Laghu build variant metadata")
endfunction()
