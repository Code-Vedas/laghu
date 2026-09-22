# SPDX-License-Identifier: AGPL-3.0-only
foreach(required IN ITEMS MANIFEST SPDX CYCLONEDX PROVENANCE)
  if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
    message(FATAL_ERROR "Laghu supply-chain validation failed: rule=missing_artifact artifact=${required}")
  endif()
endforeach()

file(READ "${MANIFEST}" manifest)
file(READ "${SPDX}" spdx)
file(READ "${CYCLONEDX}" cyclonedx)
file(READ "${PROVENANCE}" provenance)

string(JSON build_id GET "${manifest}" build_id)
string(JSON spdx_context GET "${spdx}" @context)
string(JSON cdx_format GET "${cyclonedx}" bomFormat)
string(JSON cdx_spec GET "${cyclonedx}" specVersion)
string(JSON provenance_type GET "${provenance}" _type)
string(JSON predicate_type GET "${provenance}" predicateType)
string(JSON provenance_build_id GET "${provenance}" predicate buildDefinition internalParameters buildId)
if(NOT spdx_context STREQUAL "https://spdx.org/rdf/3.0.1/spdx-context.jsonld")
  message(FATAL_ERROR "Laghu supply-chain validation failed: rule=spdx_schema_version")
endif()
if(NOT cdx_format STREQUAL "CycloneDX" OR NOT cdx_spec STREQUAL "1.7")
  message(FATAL_ERROR "Laghu supply-chain validation failed: rule=cyclonedx_schema_version")
endif()
if(NOT provenance_type STREQUAL "https://in-toto.io/Statement/v1" OR
    NOT predicate_type STREQUAL "https://slsa.dev/provenance/v1")
  message(FATAL_ERROR "Laghu supply-chain validation failed: rule=provenance_schema_version")
endif()
if(NOT provenance_build_id STREQUAL build_id)
  message(FATAL_ERROR "Laghu supply-chain validation failed: rule=build_id_mismatch")
endif()
if(DEFINED EXPECTED_CREATED)
  string(JSON graph_count LENGTH "${spdx}" @graph)
  math(EXPR graph_last "${graph_count} - 1")
  set(actual_created "")
  foreach(index RANGE 0 ${graph_last})
    string(JSON graph_type GET "${spdx}" @graph ${index} type)
    if(graph_type STREQUAL "CreationInfo")
      string(JSON actual_created GET "${spdx}" @graph ${index} created)
    endif()
  endforeach()
  if(NOT actual_created STREQUAL EXPECTED_CREATED)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=source_date_epoch expected=${EXPECTED_CREATED} actual=${actual_created}")
  endif()
endif()

foreach(required_text IN ITEMS
    "\"name\":\"${CMAKE_CXX_COMPILER_ID}\""
    "\"name\":\"${LAGHU_STANDARD_LIBRARY_ID}\""
    "\"bom-ref\":\"toolchain:compiler\""
    "\"bom-ref\":\"toolchain:standard-library\""
    "\"expression\":\"AGPL-3.0-only\""
    "\"simplelicensing_licenseExpression\":\"AGPL-3.0-only\"")
  string(FIND "${spdx}${cyclonedx}" "${required_text}" required_index)
  if(required_index EQUAL -1)
    message(FATAL_ERROR "Laghu supply-chain validation failed: rule=missing_dependency_entry value=${required_text}")
  endif()
endforeach()

string(JSON dependency_count LENGTH "${manifest}" dependencies)
string(JSON build_input_count LENGTH "${manifest}" build_inputs)
math(EXPR expected_component_count "${dependency_count} + 2")
math(EXPR expected_resolved_count "${dependency_count} + ${build_input_count}")
string(JSON component_count LENGTH "${cyclonedx}" components)
string(JSON resolved_count LENGTH "${provenance}" predicate buildDefinition resolvedDependencies)
if(NOT component_count EQUAL expected_component_count OR NOT resolved_count EQUAL expected_resolved_count)
  message(FATAL_ERROR
    "Laghu supply-chain validation failed: rule=dependency_inventory_count manifest=${dependency_count} cyclonedx=${component_count} provenance=${resolved_count}")
endif()

if(build_input_count GREATER 0)
  math(EXPR build_input_last "${build_input_count} - 1")
  foreach(index RANGE 0 ${build_input_last})
    string(JSON input_path GET "${manifest}" build_inputs ${index} path)
    string(JSON input_sha256 GET "${manifest}" build_inputs ${index} sha256)
    foreach(required_text IN ITEMS "\"uri\":\"file:${input_path}\"" "\"sha256\":\"${input_sha256}\"")
      string(FIND "${provenance}" "${required_text}" required_index)
      if(required_index EQUAL -1)
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=declared_input_missing path=${input_path}")
      endif()
    endforeach()
  endforeach()
endif()

if(dependency_count GREATER 0)
  math(EXPR dependency_last "${dependency_count} - 1")
  foreach(index RANGE 0 ${dependency_last})
    foreach(field IN ITEMS provider version url sha256 license source linkage verification)
      string(JSON ${field} GET "${manifest}" dependencies ${index} ${field})
    endforeach()
    foreach(required_text IN ITEMS
        "\"bom-ref\":\"dependency:${provider}\""
        "\"name\":\"${provider}\""
        "\"version\":\"${version}\""
        "\"expression\":\"${license}\""
        "\"value\":\"${source}\""
        "\"value\":\"${linkage}\""
        "\"value\":\"${verification}\""
        "\"simplelicensing_licenseExpression\":\"${license}\""
        "\"software_packageVersion\":\"${version}\"")
      string(FIND "${spdx}${cyclonedx}${provenance}" "${required_text}" required_index)
      if(required_index EQUAL -1)
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=missing_dependency_entry dependency=${provider} value=${required_text}")
      endif()
    endforeach()
    if(verification STREQUAL verified-archive)
      foreach(required_text IN ITEMS
          "\"content\":\"${sha256}\""
          "\"url\":\"${url}\""
          "\"software_downloadLocation\":\"${url}\""
          "\"hashValue\":\"${sha256}\""
          "\"uri\":\"${url}\"")
        string(FIND "${spdx}${cyclonedx}${provenance}" "${required_text}" required_index)
        if(required_index EQUAL -1)
          message(FATAL_ERROR
            "Laghu supply-chain validation failed: rule=verified_archive_incomplete dependency=${provider}")
        endif()
      endforeach()
    elseif(verification STREQUAL system-package-unverified)
      if(NOT url STREQUAL "" OR NOT sha256 STREQUAL "")
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=unverifiable_system_archive_claim dependency=${provider}")
      endif()
      string(FIND "${provenance}" "\"uri\":\"pkg:generic/${provider}@${version}\"" system_uri_index)
      if(system_uri_index EQUAL -1)
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=system_package_identity_missing dependency=${provider}")
      endif()
    else()
      message(FATAL_ERROR
        "Laghu supply-chain validation failed: rule=unknown_verification dependency=${provider}")
    endif()
  endforeach()
endif()

foreach(forbidden IN ITEMS "${SOURCE}" "${BINARY}")
  if(NOT forbidden STREQUAL "")
    string(FIND "${spdx}${cyclonedx}${provenance}" "${forbidden}" leaked_index)
    if(NOT leaked_index EQUAL -1)
      message(FATAL_ERROR "Laghu supply-chain validation failed: rule=absolute_path_leak path=${forbidden}")
    endif()
  endif()
endforeach()
foreach(forbidden_key IN ITEMS "\"timestamp\"" "\"startedOn\"" "\"finishedOn\"")
  string(FIND "${cyclonedx}${provenance}" "${forbidden_key}" leaked_index)
  if(NOT leaked_index EQUAL -1)
    message(FATAL_ERROR "Laghu supply-chain validation failed: rule=time_leak key=${forbidden_key}")
  endif()
endforeach()

get_filename_component(config_directory "${PROVENANCE}" DIRECTORY)
string(JSON subject_count LENGTH "${provenance}" subject)
if(NOT subject_count EQUAL 3)
  message(FATAL_ERROR "Laghu supply-chain validation failed: rule=subject_count")
endif()
math(EXPR subject_last "${subject_count} - 1")
foreach(index RANGE 0 ${subject_last})
  string(JSON subject_name GET "${provenance}" subject ${index} name)
  string(JSON subject_sha256 GET "${provenance}" subject ${index} digest sha256)
  set(subject_path "${config_directory}/${subject_name}")
  if(NOT EXISTS "${subject_path}")
    message(FATAL_ERROR "Laghu supply-chain validation failed: rule=subject_missing subject=${subject_name}")
  endif()
  file(SHA256 "${subject_path}" actual_sha256)
  if(NOT actual_sha256 STREQUAL subject_sha256)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=subject_hash_mismatch subject=${subject_name} expected=${subject_sha256} actual=${actual_sha256}")
  endif()
endforeach()
