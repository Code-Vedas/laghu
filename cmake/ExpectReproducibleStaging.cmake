# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED CXX)
  message(FATAL_ERROR "Laghu reproducibility expectation requires LAGHU_SOURCE and CXX")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/reproducible-staging")
set(compare_script "${LAGHU_SOURCE}/cmake/CompareReproducibleArtifact.cmake")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(laghu_copy_source destination)
  foreach(entry IN ITEMS CMakeLists.txt CMakePresets.json VERSION bench cmake docs src tests)
    set(source_entry "${LAGHU_SOURCE}/${entry}")
    if(NOT EXISTS "${source_entry}")
      message(FATAL_ERROR "Laghu reproducibility expectation failed: source_entry_missing=${entry}")
    endif()
    file(COPY "${source_entry}" DESTINATION "${destination}")
  endforeach()
endfunction()

function(laghu_configure_build_and_stage name)
  set(source "${work}/source-${name}")
  set(binary "${work}/build-${name}")
  set(stage "${work}/stage-${name}")
  set(prefix "/opt/laghu-reproducible")
  file(MAKE_DIRECTORY "${source}")
  laghu_copy_source("${source}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${binary}" -G Ninja
      "-DCMAKE_CXX_COMPILER=${CXX}"
      "-DCMAKE_CXX_FLAGS=${CXXFLAGS}"
      -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_INSTALL_PREFIX=${prefix}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
  if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
      "Laghu reproducibility expectation failed: configure_failed build=${name}\n${configure_output}${configure_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${binary}" --target laghu_core laghu
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
      "Laghu reproducibility expectation failed: build_failed build=${name}\n${build_output}${build_error}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}"
      "${CMAKE_COMMAND}" --install "${binary}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
  if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
      "Laghu reproducibility expectation failed: install_failed build=${name}\n${install_output}${install_error}")
  endif()
  set("LAGHU_REPRO_SOURCE_${name}" "${source}" PARENT_SCOPE)
  set("LAGHU_REPRO_BINARY_${name}" "${binary}" PARENT_SCOPE)
  set("LAGHU_REPRO_STAGE_${name}" "${stage}${prefix}" PARENT_SCOPE)
endfunction()

function(laghu_assert_no_path_leak artifact source binary)
  file(READ "${artifact}" artifact_hex HEX)
  foreach(forbidden IN ITEMS "${source}" "${binary}" "${binary}/generated")
    string(HEX "${forbidden}" forbidden_hex)
    string(FIND "${artifact_hex}" "${forbidden_hex}" leak_offset)
    if(NOT leak_offset EQUAL -1)
      message(FATAL_ERROR
        "Laghu reproducibility expectation failed: artifact_path_leak artifact=${artifact} leaked_path=${forbidden}")
    endif()
  endforeach()
endfunction()

function(laghu_compare artifact first second)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DARTIFACT=${artifact}"
      "-DFIRST=${first}"
      "-DSECOND=${second}"
      -P "${compare_script}"
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_output
    ERROR_VARIABLE compare_error)
  if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR
      "Laghu reproducibility expectation failed: equivalent_artifacts_differ\n${compare_output}${compare_error}")
  endif()
endfunction()

laghu_configure_build_and_stage(a)
laghu_configure_build_and_stage(b)

set(artifacts
  "cli|${LAGHU_REPRO_STAGE_a}/bin/laghu|${LAGHU_REPRO_STAGE_b}/bin/laghu"
  "archive|${LAGHU_REPRO_STAGE_a}/lib/laghu/liblaghu_core.a|${LAGHU_REPRO_STAGE_b}/lib/laghu/liblaghu_core.a"
  "manifest|${LAGHU_REPRO_STAGE_a}/share/laghu/laghu-build-manifest-v1.json|${LAGHU_REPRO_STAGE_b}/share/laghu/laghu-build-manifest-v1.json"
  "capabilities_header|${LAGHU_REPRO_BINARY_a}/generated/laghu/capabilities.hpp|${LAGHU_REPRO_BINARY_b}/generated/laghu/capabilities.hpp")
foreach(entry IN LISTS artifacts)
  string(REPLACE "|" ";" fields "${entry}")
  list(GET fields 0 artifact)
  list(GET fields 1 first)
  list(GET fields 2 second)
  laghu_assert_no_path_leak("${first}" "${LAGHU_REPRO_SOURCE_a}" "${LAGHU_REPRO_BINARY_a}")
  laghu_assert_no_path_leak("${second}" "${LAGHU_REPRO_SOURCE_b}" "${LAGHU_REPRO_BINARY_b}")
  laghu_compare("${artifact}" "${first}" "${second}")
endforeach()

set(seed_source "${LAGHU_REPRO_STAGE_b}/share/laghu/laghu-build-manifest-v1.json")
set(seed_artifact "${work}/seeded-nondeterminism-manifest.json")
file(COPY "${seed_source}" DESTINATION "${work}")
file(RENAME "${work}/laghu-build-manifest-v1.json" "${seed_artifact}")
file(APPEND "${seed_artifact}" "seeded-nondeterminism\n")
file(SHA256 "${LAGHU_REPRO_STAGE_a}/share/laghu/laghu-build-manifest-v1.json" seed_first_sha256)
file(SHA256 "${seed_artifact}" seed_second_sha256)
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -DARTIFACT=manifest
    "-DFIRST=${LAGHU_REPRO_STAGE_a}/share/laghu/laghu-build-manifest-v1.json"
    "-DSECOND=${seed_artifact}"
    -P "${compare_script}"
  RESULT_VARIABLE seed_result
  OUTPUT_VARIABLE seed_output
  ERROR_VARIABLE seed_error)
if(seed_result EQUAL 0)
  message(FATAL_ERROR "Laghu reproducibility expectation failed: seeded_nondeterminism_not_detected")
endif()
set(seed_diagnostic "${seed_output}${seed_error}")
foreach(expected IN ITEMS
    "artifact=manifest"
    "path_a=${LAGHU_REPRO_STAGE_a}/share/laghu/laghu-build-manifest-v1.json"
    "sha256_a=${seed_first_sha256}"
    "path_b=${seed_artifact}"
    "sha256_b=${seed_second_sha256}")
  string(FIND "${seed_diagnostic}" "${expected}" expected_offset)
  if(expected_offset EQUAL -1)
    message(FATAL_ERROR
      "Laghu reproducibility expectation failed: seeded_diagnostic_missing expected=${expected}\n${seed_diagnostic}")
  endif()
endforeach()
