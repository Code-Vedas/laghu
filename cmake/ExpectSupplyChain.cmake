# SPDX-License-Identifier: AGPL-3.0-only
foreach(required IN ITEMS MANIFEST SPDX CYCLONEDX PROVENANCE VALIDATOR SOURCE BINARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Laghu supply-chain expectation requires ${required}")
  endif()
endforeach()
file(READ "${MANIFEST}" manifest)

execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DMANIFEST=${MANIFEST}" "-DSPDX=${SPDX}" "-DCYCLONEDX=${CYCLONEDX}"
  "-DPROVENANCE=${PROVENANCE}" "-DSOURCE=${SOURCE}" "-DBINARY=${BINARY}"
  "-DEXPECTED_CREATED=${EXPECTED_CREATED}"
  "-DCMAKE_CXX_COMPILER_ID=${COMPILER_ID}"
  "-DLAGHU_STANDARD_LIBRARY_ID=${STANDARD_LIBRARY_ID}"
  -P "${VALIDATOR}"
  RESULT_VARIABLE positive_result OUTPUT_VARIABLE positive_output ERROR_VARIABLE positive_error)
if(NOT positive_result EQUAL 0)
  message(FATAL_ERROR "Laghu supply-chain expectation failed: positive_validation\n${positive_output}${positive_error}")
endif()

set(fixture "${BINARY}/tests/supply-chain-fixtures")
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}")
foreach(file IN ITEMS "${MANIFEST}" "${SPDX}" "${CYCLONEDX}" "${PROVENANCE}")
  file(COPY "${file}" DESTINATION "${fixture}")
endforeach()
get_filename_component(manifest_name "${MANIFEST}" NAME)
get_filename_component(spdx_name "${SPDX}" NAME)
get_filename_component(cyclonedx_name "${CYCLONEDX}" NAME)
get_filename_component(provenance_name "${PROVENANCE}" NAME)

function(laghu_expect_invalid name file search replacement diagnostic)
  file(READ "${fixture}/${file}" original)
  string(REPLACE "${search}" "${replacement}" mutated "${original}")
  if(mutated STREQUAL original)
    message(FATAL_ERROR "Laghu supply-chain expectation failed: mutation_not_applied fixture=${name}")
  endif()
  file(WRITE "${fixture}/${file}" "${mutated}")
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DMANIFEST=${fixture}/${manifest_name}"
    "-DSPDX=${fixture}/${spdx_name}"
    "-DCYCLONEDX=${fixture}/${cyclonedx_name}"
    "-DPROVENANCE=${fixture}/${provenance_name}"
    "-DSOURCE=${SOURCE}" "-DBINARY=${BINARY}"
    "-DEXPECTED_CREATED=${EXPECTED_CREATED}"
    "-DCMAKE_CXX_COMPILER_ID=${COMPILER_ID}"
    "-DLAGHU_STANDARD_LIBRARY_ID=${STANDARD_LIBRARY_ID}"
    -P "${VALIDATOR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  file(WRITE "${fixture}/${file}" "${original}")
  if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${diagnostic}")
    message(FATAL_ERROR
      "Laghu supply-chain expectation failed: negative_fixture=${name} diagnostic=${diagnostic}\n${output}${error}")
  endif()
endfunction()

laghu_expect_invalid(schema "${cyclonedx_name}" "\"specVersion\":\"1.7\""
  "\"specVersion\":\"1.6\"" "rule=cyclonedx_schema_version")
laghu_expect_invalid(missing-dependency "${cyclonedx_name}"
  "\"bom-ref\":\"toolchain:compiler\"" "\"bom-ref\":\"toolchain:missing\""
  "rule=missing_dependency_entry")
laghu_expect_invalid(path-leak "${cyclonedx_name}" "{\"bomFormat\""
  "{\"leak\":\"${SOURCE}\",\"bomFormat\"" "rule=absolute_path_leak")
laghu_expect_invalid(time-leak "${provenance_name}" "{\"_type\""
  "{\"timestamp\":\"2026-01-01T00:00:00Z\",\"_type\"" "rule=time_leak")
laghu_expect_invalid(tampered-subject "${cyclonedx_name}" "{\"bomFormat\""
  "{ \"bomFormat\"" "rule=subject_hash_mismatch")
laghu_expect_invalid(duplicate-subject "${provenance_name}"
  "\"name\":\"laghu-spdx-3.0.1.spdx.json\""
  "\"name\":\"laghu-build-manifest-v1.json\"" "rule=subject_name")
laghu_expect_invalid(traversing-subject "${provenance_name}"
  "\"name\":\"laghu-spdx-3.0.1.spdx.json\""
  "\"name\":\"../laghu-spdx-3.0.1.spdx.json\"" "rule=subject_name")

string(JSON build_input_count LENGTH "${manifest}" build_inputs)
if(build_input_count GREATER 1)
  string(JSON input_path GET "${manifest}" build_inputs 0 path)
  string(JSON input_sha256 GET "${manifest}" build_inputs 0 sha256)
  string(JSON other_input_sha256 GET "${manifest}" build_inputs 1 sha256)
  laghu_expect_invalid(mispaired-input-digest "${provenance_name}"
    "\"digest\":{\"sha256\":\"${input_sha256}\"},\"uri\":\"file:${input_path}\""
    "\"digest\":{\"sha256\":\"${other_input_sha256}\"},\"unrelated\":{\"sha256\":\"${input_sha256}\"},\"uri\":\"file:${input_path}\""
    "rule=declared_input_digest_mismatch")
endif()

string(JSON dependency_count LENGTH "${manifest}" dependencies)
if(dependency_count GREATER 0)
  string(JSON provider GET "${manifest}" dependencies 0 provider)
  string(JSON version GET "${manifest}" dependencies 0 version)
  laghu_expect_invalid(misbound-cyclonedx-component "${cyclonedx_name}"
    "\"version\":\"${version}\"" "\"version\":\"invalid-${version}\""
    "rule=dependency_cyclonedx_mismatch")
endif()

if(dependency_count GREATER 1)
  set(verified_indices)
  math(EXPR dependency_last "${dependency_count} - 1")
  foreach(index RANGE 0 ${dependency_last})
    string(JSON verification GET "${manifest}" dependencies ${index} verification)
    if(verification STREQUAL verified-archive)
      list(APPEND verified_indices ${index})
    endif()
  endforeach()
  list(LENGTH verified_indices verified_count)
  if(verified_count GREATER 1)
    list(GET verified_indices 0 dependency_index)
    list(GET verified_indices 1 other_dependency_index)
    string(JSON url GET "${manifest}" dependencies ${dependency_index} url)
    string(JSON sha256 GET "${manifest}" dependencies ${dependency_index} sha256)
    string(JSON other_sha256 GET "${manifest}" dependencies ${other_dependency_index} sha256)
    laghu_expect_invalid(mispaired-dependency-digest "${provenance_name}"
      "\"digest\":{\"sha256\":\"${sha256}\"},\"uri\":\"${url}\""
      "\"digest\":{\"sha256\":\"${other_sha256}\"},\"unrelated\":{\"sha256\":\"${sha256}\"},\"uri\":\"${url}\""
      "rule=dependency_provenance_digest_mismatch")
  endif()
endif()
