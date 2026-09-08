# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE OR NOT DEFINED LAGHU_SOURCE OR NOT DEFINED FIXTURE)
  message(FATAL_ERROR "Laghu API boundary expectation requires MODULE, LAGHU_SOURCE, and FIXTURE")
endif()
get_filename_component(fixture_name "${FIXTURE}" NAME)
if(fixture_name STREQUAL "external-c-type")
  set(expected_rule "rule=external_c_header")
elseif(fixture_name STREQUAL "external-c-type-token")
  set(expected_rule "rule=external_c_type")
elseif(fixture_name STREQUAL "private-namespace")
  set(expected_rule "rule=private_namespace")
elseif(fixture_name STREQUAL "private-include")
  set(expected_rule "rule=private_header_leak")
elseif(fixture_name STREQUAL "cross-private-source")
  set(expected_rule "rule=private_header_owner")
elseif(fixture_name STREQUAL "direct-private-path")
  set(expected_rule "rule=private_header_direct_path")
elseif(fixture_name STREQUAL "legacy-global-symbol")
  set(expected_rule "rule=legacy_global_symbol")
else()
  message(FATAL_ERROR "Laghu API boundary expectation has unknown fixture=${fixture_name}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DMODULE=${MODULE}"
    "-DLAGHU_SOURCE=${LAGHU_SOURCE}"
    "-DFIXTURE=${FIXTURE}"
    -P "${LAGHU_SOURCE}/cmake/RunApiBoundaryPolicy.cmake"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics)
if(result EQUAL 0)
  message(FATAL_ERROR "Laghu API boundary fixture unexpectedly passed: fixture=${fixture_name}")
endif()
set(combined "${output}\n${diagnostics}")
if(NOT combined MATCHES "${expected_rule}")
  message(FATAL_ERROR
    "Laghu API boundary fixture diagnostic missing: fixture=${fixture_name}; expected=${expected_rule}\n${combined}")
endif()
