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

function(laghu_find_json_object output json key expected)
  set(path ${ARGN})
  string(JSON object_count LENGTH "${json}" ${path})
  set(found_index -1)
  set(match_count 0)
  if(object_count GREATER 0)
    math(EXPR object_last "${object_count} - 1")
    foreach(object_index RANGE 0 ${object_last})
      string(JSON actual ERROR_VARIABLE field_error GET
        "${json}" ${path} ${object_index} ${key})
      if(field_error STREQUAL NOTFOUND AND actual STREQUAL expected)
        set(found_index ${object_index})
        math(EXPR match_count "${match_count} + 1")
      endif()
    endforeach()
  endif()
  if(NOT match_count EQUAL 1)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=json_identity key=${key} value=${expected} matches=${match_count}")
  endif()
  set(${output} ${found_index} PARENT_SCOPE)
endfunction()

function(laghu_require_json_value json expected diagnostic)
  string(JSON actual ERROR_VARIABLE value_error GET "${json}" ${ARGN})
  if(NOT value_error STREQUAL NOTFOUND OR NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=${diagnostic} expected=${expected} actual=${actual}")
  endif()
endfunction()

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
    laghu_find_json_object(resolved_index "${provenance}" uri "file:${input_path}"
      predicate buildDefinition resolvedDependencies)
    laghu_require_json_value("${provenance}" "${input_sha256}"
      declared_input_digest_mismatch predicate buildDefinition resolvedDependencies
      ${resolved_index} digest sha256)
  endforeach()
endif()

if(dependency_count GREATER 0)
  laghu_find_json_object(document_index "${spdx}" type SpdxDocument @graph)
  string(JSON document_id GET "${spdx}" @graph ${document_index} spdxId)
  string(JSON external_dependency_count LENGTH "${provenance}"
    predicate buildDefinition externalParameters dependencies)
  if(NOT external_dependency_count EQUAL dependency_count)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=dependency_inventory_count manifest=${dependency_count} external=${external_dependency_count}")
  endif()
  math(EXPR dependency_last "${dependency_count} - 1")
  foreach(index RANGE 0 ${dependency_last})
    foreach(field IN ITEMS provider version url sha256 license source linkage verification)
      string(JSON ${field} GET "${manifest}" dependencies ${index} ${field})
    endforeach()

    laghu_find_json_object(external_index "${provenance}" provider "${provider}"
      predicate buildDefinition externalParameters dependencies)
    foreach(field IN ITEMS version url sha256 license source linkage verification)
      laghu_require_json_value("${provenance}" "${${field}}"
        dependency_external_mismatch predicate buildDefinition externalParameters
        dependencies ${external_index} ${field})
    endforeach()

    laghu_find_json_object(component_index "${cyclonedx}" bom-ref
      "dependency:${provider}" components)
    laghu_require_json_value("${cyclonedx}" "${provider}"
      dependency_cyclonedx_mismatch components ${component_index} name)
    laghu_require_json_value("${cyclonedx}" library
      dependency_cyclonedx_mismatch components ${component_index} type)
    laghu_require_json_value("${cyclonedx}" "${version}"
      dependency_cyclonedx_mismatch components ${component_index} version)
    laghu_require_json_value("${cyclonedx}" "${license}"
      dependency_cyclonedx_mismatch components ${component_index} licenses 0 expression)
    foreach(property_name IN ITEMS laghu:linkage laghu:source laghu:verification)
      if(property_name STREQUAL laghu:linkage)
        set(property_value "${linkage}")
      elseif(property_name STREQUAL laghu:source)
        set(property_value "${source}")
      else()
        set(property_value "${verification}")
      endif()
      laghu_find_json_object(property_index "${cyclonedx}" name "${property_name}"
        components ${component_index} properties)
      laghu_require_json_value("${cyclonedx}" "${property_value}"
        dependency_cyclonedx_mismatch components ${component_index} properties
        ${property_index} value)
    endforeach()

    set(package_id "${document_id}#dependency-${provider}")
    set(license_id "${document_id}#license-${provider}")
    laghu_find_json_object(package_index "${spdx}" spdxId "${package_id}" @graph)
    laghu_require_json_value("${spdx}" "${provider}"
      dependency_spdx_mismatch @graph ${package_index} name)
    laghu_require_json_value("${spdx}" software_Package
      dependency_spdx_mismatch @graph ${package_index} type)
    laghu_require_json_value("${spdx}" "${version}"
      dependency_spdx_mismatch @graph ${package_index} software_packageVersion)
    laghu_find_json_object(license_index "${spdx}" spdxId "${license_id}" @graph)
    laghu_require_json_value("${spdx}" simplelicensing_LicenseExpression
      dependency_spdx_mismatch @graph ${license_index} type)
    laghu_require_json_value("${spdx}" "${license}"
      dependency_spdx_mismatch @graph ${license_index} simplelicensing_licenseExpression)

    if(verification STREQUAL verified-archive)
      laghu_require_json_value("${cyclonedx}" "${url}"
        dependency_cyclonedx_mismatch components ${component_index}
        externalReferences 0 url)
      laghu_require_json_value("${cyclonedx}" distribution
        dependency_cyclonedx_mismatch components ${component_index}
        externalReferences 0 type)
      laghu_require_json_value("${cyclonedx}" "${sha256}"
        dependency_cyclonedx_mismatch components ${component_index} hashes 0 content)
      laghu_require_json_value("${cyclonedx}" SHA-256
        dependency_cyclonedx_mismatch components ${component_index} hashes 0 alg)
      laghu_require_json_value("${spdx}" "${url}"
        dependency_spdx_mismatch @graph ${package_index} software_downloadLocation)
      laghu_require_json_value("${spdx}" "${sha256}"
        dependency_spdx_mismatch @graph ${package_index} verifiedUsing 0 hashValue)
      laghu_require_json_value("${spdx}" sha256
        dependency_spdx_mismatch @graph ${package_index} verifiedUsing 0 algorithm)
      laghu_require_json_value("${spdx}" Hash
        dependency_spdx_mismatch @graph ${package_index} verifiedUsing 0 type)
      laghu_find_json_object(resolved_index "${provenance}" uri "${url}"
        predicate buildDefinition resolvedDependencies)
      laghu_require_json_value("${provenance}" "${sha256}"
        dependency_provenance_digest_mismatch predicate buildDefinition
        resolvedDependencies ${resolved_index} digest sha256)
    elseif(verification STREQUAL system-package-unverified)
      if(NOT url STREQUAL "" OR NOT sha256 STREQUAL "")
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=unverifiable_system_archive_claim dependency=${provider}")
      endif()
      laghu_find_json_object(resolved_index "${provenance}" uri
        "pkg:generic/${provider}@${version}"
        predicate buildDefinition resolvedDependencies)
      string(JSON unexpected_digest ERROR_VARIABLE digest_error GET "${provenance}"
        predicate buildDefinition resolvedDependencies ${resolved_index} digest)
      if(digest_error STREQUAL NOTFOUND)
        message(FATAL_ERROR
          "Laghu supply-chain validation failed: rule=unverifiable_system_digest dependency=${provider}")
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
set(expected_subjects
  laghu-build-manifest-v1.json
  laghu-cyclonedx-1.7.cdx.json
  laghu-spdx-3.0.1.spdx.json)
math(EXPR subject_last "${subject_count} - 1")
foreach(index RANGE 0 ${subject_last})
  string(JSON subject_name GET "${provenance}" subject ${index} name)
  string(JSON subject_sha256 GET "${provenance}" subject ${index} digest sha256)
  list(FIND expected_subjects "${subject_name}" expected_subject_index)
  if(expected_subject_index EQUAL -1)
    message(FATAL_ERROR
      "Laghu supply-chain validation failed: rule=subject_name subject=${subject_name}")
  endif()
  list(REMOVE_AT expected_subjects ${expected_subject_index})
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
if(expected_subjects)
  message(FATAL_ERROR
    "Laghu supply-chain validation failed: rule=subject_missing subjects=${expected_subjects}")
endif()
