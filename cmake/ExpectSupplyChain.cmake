# SPDX-License-Identifier: AGPL-3.0-only
foreach(required IN ITEMS MANIFEST SPDX CYCLONEDX PROVENANCE VALIDATOR SOURCE BINARY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Laghu supply-chain expectation requires ${required}")
  endif()
endforeach()

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
