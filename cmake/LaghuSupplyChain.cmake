# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_SPDX_SCHEMA_VERSION 3.0.1)
set(LAGHU_CYCLONEDX_SCHEMA_VERSION 1.7)
set(LAGHU_PROVENANCE_PREDICATE_TYPE https://slsa.dev/provenance/v1)

function(laghu_json_quote value output)
  string(REPLACE "\\" "\\\\" escaped "${value}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\n" "\\n" escaped "${escaped}")
  string(REPLACE "\r" "\\r" escaped "${escaped}")
  string(REPLACE "\t" "\\t" escaped "${escaped}")
  set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

function(laghu_supply_chain_timestamp output)
  if(DEFINED ENV{SOURCE_DATE_EPOCH} AND NOT "$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
    if(NOT "$ENV{SOURCE_DATE_EPOCH}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "Laghu supply-chain generation failed: rule=invalid_source_date_epoch")
    endif()
    string(TIMESTAMP created "%Y-%m-%dT%H:%M:%SZ" UTC)
  else()
    # SPDX CreationInfo requires a timestamp. This fixed epoch is metadata,
    # not an assertion that the build occurred at that instant.
    set(created 1970-01-01T00:00:00Z)
  endif()
  set(${output} "${created}" PARENT_SCOPE)
endfunction()

function(laghu_configure_supply_chain)
  if(NOT DEFINED LAGHU_BUILD_ID OR NOT EXISTS "${LAGHU_BUILD_MANIFEST}")
    message(FATAL_ERROR "Laghu supply-chain generation requires the canonical build identity")
  endif()

  laghu_supply_chain_timestamp(created)
  set(namespace "https://codevedas.com/laghu/spdx/${LAGHU_BUILD_ID}")
  set(creation_id "${namespace}#creation-info")
  set(creator_id "${namespace}#codevedas")
  set(document_id "${namespace}")
  set(sbom_id "${namespace}#sbom")
  set(laghu_id "${namespace}#laghu")

  set(spdx_graph
    "{\"@id\":\"${creation_id}\",\"created\":\"${created}\",\"createdBy\":[\"${creator_id}\"],\"specVersion\":\"${LAGHU_SPDX_SCHEMA_VERSION}\",\"type\":\"CreationInfo\"}"
    "{\"creationInfo\":\"${creation_id}\",\"name\":\"Code Vedas\",\"spdxId\":\"${creator_id}\",\"type\":\"Organization\"}"
    "{\"creationInfo\":\"${creation_id}\",\"element\":[\"${sbom_id}\"],\"profileConformance\":[\"core\",\"simpleLicensing\",\"software\"],\"rootElement\":[\"${sbom_id}\"],\"spdxId\":\"${document_id}\",\"type\":\"SpdxDocument\"}"
    "{\"creationInfo\":\"${creation_id}\",\"element\":[\"${laghu_id}\"],\"rootElement\":[\"${laghu_id}\"],\"software_sbomType\":[\"build\"],\"spdxId\":\"${sbom_id}\",\"type\":\"software_Sbom\"}"
    "{\"creationInfo\":\"${creation_id}\",\"name\":\"laghu\",\"software_packageVersion\":\"${LAGHU_PRODUCT_VERSION}\",\"spdxId\":\"${laghu_id}\",\"type\":\"software_Package\"}"
    "{\"creationInfo\":\"${creation_id}\",\"from\":\"${laghu_id}\",\"relationshipType\":\"hasDeclaredLicense\",\"spdxId\":\"${namespace}#relationship-license-laghu\",\"to\":[\"${namespace}#license-laghu\"],\"type\":\"Relationship\"}"
    "{\"creationInfo\":\"${creation_id}\",\"simplelicensing_licenseExpression\":\"AGPL-3.0-only\",\"spdxId\":\"${namespace}#license-laghu\",\"type\":\"simplelicensing_LicenseExpression\"}"
    "{\"creationInfo\":\"${creation_id}\",\"name\":\"${CMAKE_CXX_COMPILER_ID}\",\"software_packageVersion\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"spdxId\":\"${namespace}#toolchain-compiler\",\"type\":\"software_Package\"}"
    "{\"creationInfo\":\"${creation_id}\",\"from\":\"${laghu_id}\",\"relationshipType\":\"dependsOn\",\"spdxId\":\"${namespace}#relationship-toolchain-compiler\",\"to\":[\"${namespace}#toolchain-compiler\"],\"type\":\"Relationship\"}"
    "{\"creationInfo\":\"${creation_id}\",\"name\":\"${LAGHU_STANDARD_LIBRARY_ID}\",\"software_packageVersion\":\"${LAGHU_STANDARD_LIBRARY_VERSION}\",\"spdxId\":\"${namespace}#toolchain-standard-library\",\"type\":\"software_Package\"}"
    "{\"creationInfo\":\"${creation_id}\",\"from\":\"${laghu_id}\",\"relationshipType\":\"dependsOn\",\"spdxId\":\"${namespace}#relationship-toolchain-standard-library\",\"to\":[\"${namespace}#toolchain-standard-library\"],\"type\":\"Relationship\"}")

  laghu_json_quote("${CMAKE_CXX_COMPILER_ID}" compiler_id_json)
  laghu_json_quote("${CMAKE_CXX_COMPILER_VERSION}" compiler_version_json)
  laghu_json_quote("${LAGHU_STANDARD_LIBRARY_ID}" standard_library_id_json)
  laghu_json_quote("${LAGHU_STANDARD_LIBRARY_VERSION}" standard_library_version_json)
  set(cdx_components
    "{\"bom-ref\":\"toolchain:compiler\",\"name\":${compiler_id_json},\"properties\":[{\"name\":\"laghu:component-kind\",\"value\":\"compiler\"}],\"type\":\"application\",\"version\":${compiler_version_json}}"
    "{\"bom-ref\":\"toolchain:standard-library\",\"name\":${standard_library_id_json},\"properties\":[{\"name\":\"laghu:component-kind\",\"value\":\"standard-library\"}],\"type\":\"library\",\"version\":${standard_library_version_json}}")
  set(cdx_dependency_refs "\"toolchain:compiler\"" "\"toolchain:standard-library\"")
  set(provenance_dependencies)

  file(READ "${LAGHU_BUILD_MANIFEST}" manifest)
  string(JSON build_input_count LENGTH "${manifest}" build_inputs)
  if(build_input_count GREATER 0)
    math(EXPR build_input_last "${build_input_count} - 1")
    foreach(index RANGE 0 ${build_input_last})
      string(JSON input_path GET "${manifest}" build_inputs ${index} path)
      string(JSON input_sha256 GET "${manifest}" build_inputs ${index} sha256)
      list(APPEND provenance_dependencies
        "{\"digest\":{\"sha256\":\"${input_sha256}\"},\"uri\":\"file:${input_path}\"}")
      if(input_path STREQUAL ".github/workflows/toolchain.yml")
        set(workflow_sha256 "${input_sha256}")
      endif()
    endforeach()
  endif()
  if(workflow_sha256 STREQUAL "")
    message(FATAL_ERROR "Laghu supply-chain generation failed: rule=workflow_identity_missing")
  endif()

  string(JSON dependency_count LENGTH "${manifest}" dependencies)
  if(dependency_count GREATER 0)
    math(EXPR dependency_last "${dependency_count} - 1")
    foreach(dependency_index RANGE 0 ${dependency_last})
      foreach(field IN ITEMS provider source version linkage url sha256 license verification)
        string(JSON ${field} GET "${manifest}" dependencies ${dependency_index} ${field})
      endforeach()
      set(id "${provider}")

      set(package_id "${namespace}#dependency-${id}")
      set(license_id "${namespace}#license-${id}")
      if(verification STREQUAL verified-archive)
        set(spdx_package "{\"creationInfo\":\"${creation_id}\",\"name\":\"${id}\",\"software_downloadLocation\":\"${url}\",\"software_packageVersion\":\"${version}\",\"spdxId\":\"${package_id}\",\"type\":\"software_Package\",\"verifiedUsing\":[{\"algorithm\":\"sha256\",\"hashValue\":\"${sha256}\",\"type\":\"Hash\"}]}")
        set(cdx_verification "\"externalReferences\":[{\"type\":\"distribution\",\"url\":\"${url}\"}],\"hashes\":[{\"alg\":\"SHA-256\",\"content\":\"${sha256}\"}],")
        set(provenance_dependency "{\"digest\":{\"sha256\":\"${sha256}\"},\"uri\":\"${url}\"}")
      else()
        set(spdx_package "{\"comment\":\"System package origin and archive digest are not verifiable from the compiler/linker interface\",\"creationInfo\":\"${creation_id}\",\"name\":\"${id}\",\"software_packageVersion\":\"${version}\",\"spdxId\":\"${package_id}\",\"type\":\"software_Package\"}")
        set(cdx_verification "")
        set(provenance_dependency "{\"uri\":\"pkg:generic/${id}@${version}\"}")
      endif()
      list(APPEND spdx_graph
        "${spdx_package}"
        "{\"creationInfo\":\"${creation_id}\",\"from\":\"${laghu_id}\",\"relationshipType\":\"dependsOn\",\"spdxId\":\"${namespace}#relationship-dependency-${id}\",\"to\":[\"${package_id}\"],\"type\":\"Relationship\"}"
        "{\"creationInfo\":\"${creation_id}\",\"from\":\"${package_id}\",\"relationshipType\":\"hasDeclaredLicense\",\"spdxId\":\"${namespace}#relationship-license-${id}\",\"to\":[\"${license_id}\"],\"type\":\"Relationship\"}"
        "{\"creationInfo\":\"${creation_id}\",\"simplelicensing_licenseExpression\":\"${license}\",\"spdxId\":\"${license_id}\",\"type\":\"simplelicensing_LicenseExpression\"}")
      list(APPEND cdx_components
        "{\"bom-ref\":\"dependency:${id}\",${cdx_verification}\"licenses\":[{\"expression\":\"${license}\"}],\"name\":\"${id}\",\"properties\":[{\"name\":\"laghu:linkage\",\"value\":\"${linkage}\"},{\"name\":\"laghu:source\",\"value\":\"${source}\"},{\"name\":\"laghu:verification\",\"value\":\"${verification}\"}],\"type\":\"library\",\"version\":\"${version}\"}")
      list(APPEND cdx_dependency_refs "\"dependency:${id}\"")
      list(APPEND provenance_dependencies "${provenance_dependency}")
    endforeach()
  endif()

  list(SORT spdx_graph)
  list(SORT cdx_components)
  list(SORT cdx_dependency_refs)
  list(SORT provenance_dependencies)
  list(JOIN spdx_graph "," spdx_graph_json)
  set(spdx "{\"@context\":\"https://spdx.org/rdf/${LAGHU_SPDX_SCHEMA_VERSION}/spdx-context.jsonld\",\"@graph\":[${spdx_graph_json}]}")

  list(JOIN cdx_components "," cdx_components_json)
  list(JOIN cdx_dependency_refs "," cdx_dependency_refs_json)
  set(cdx_dependencies "{\"dependsOn\":[${cdx_dependency_refs_json}],\"ref\":\"pkg:generic/laghu@${LAGHU_PRODUCT_VERSION}\"}")
  foreach(reference IN LISTS cdx_dependency_refs)
    string(REPLACE "\"" "" reference_text "${reference}")
    string(APPEND cdx_dependencies ",{\"dependsOn\":[],\"ref\":\"${reference_text}\"}")
  endforeach()
  set(cyclonedx "{\"bomFormat\":\"CycloneDX\",\"components\":[${cdx_components_json}],\"dependencies\":[${cdx_dependencies}],\"metadata\":{\"component\":{\"bom-ref\":\"pkg:generic/laghu@${LAGHU_PRODUCT_VERSION}\",\"licenses\":[{\"expression\":\"AGPL-3.0-only\"}],\"name\":\"laghu\",\"properties\":[{\"name\":\"laghu:build-id\",\"value\":\"${LAGHU_BUILD_ID}\"}],\"type\":\"application\",\"version\":\"${LAGHU_PRODUCT_VERSION}\"}},\"specVersion\":\"${LAGHU_CYCLONEDX_SCHEMA_VERSION}\",\"version\":1}")

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  set(spdx_path "${CMAKE_BINARY_DIR}/config/laghu-spdx-${LAGHU_SPDX_SCHEMA_VERSION}.spdx.json")
  set(cyclonedx_path "${CMAKE_BINARY_DIR}/config/laghu-cyclonedx-${LAGHU_CYCLONEDX_SCHEMA_VERSION}.cdx.json")
  file(WRITE "${spdx_path}" "${spdx}")
  file(WRITE "${cyclonedx_path}" "${cyclonedx}")
  file(SHA256 "${LAGHU_BUILD_MANIFEST}" manifest_sha256)
  file(SHA256 "${spdx_path}" spdx_sha256)
  file(SHA256 "${cyclonedx_path}" cyclonedx_sha256)
  list(JOIN provenance_dependencies "," provenance_dependencies_json)
  set(requested_features ${LAGHU_BUILD_REQUESTED_FEATURES})
  set(effective_features ${LAGHU_BUILD_EFFECTIVE_FEATURES})
  list(SORT requested_features)
  list(SORT effective_features)
  laghu_build_identity_json_array(requested_features_json ${requested_features})
  laghu_build_identity_json_array(effective_features_json ${effective_features})
  set(provenance "{\"_type\":\"https://in-toto.io/Statement/v1\",\"predicate\":{\"buildDefinition\":{\"buildType\":\"https://codevedas.com/laghu/build/v1\",\"externalParameters\":{\"buildProfile\":\"${LAGHU_BUILD_PROFILE}\",\"dependencies\":${LAGHU_BUILD_IDENTITY_DEPENDENCIES_JSON},\"effectiveFeatures\":${effective_features_json},\"requestedFeatures\":${requested_features_json},\"workflow\":{\"path\":\".github/workflows/toolchain.yml\",\"sha256\":\"${workflow_sha256}\"}},\"internalParameters\":{\"buildId\":\"${LAGHU_BUILD_ID}\"},\"resolvedDependencies\":[${provenance_dependencies_json}]},\"runDetails\":{\"builder\":{\"id\":\"https://codevedas.com/laghu/cmake\"},\"metadata\":{\"invocationId\":\"${LAGHU_BUILD_ID}\"}}},\"predicateType\":\"${LAGHU_PROVENANCE_PREDICATE_TYPE}\",\"subject\":[{\"digest\":{\"sha256\":\"${cyclonedx_sha256}\"},\"name\":\"laghu-cyclonedx-${LAGHU_CYCLONEDX_SCHEMA_VERSION}.cdx.json\"},{\"digest\":{\"sha256\":\"${manifest_sha256}\"},\"name\":\"laghu-build-manifest-v1.json\"},{\"digest\":{\"sha256\":\"${spdx_sha256}\"},\"name\":\"laghu-spdx-${LAGHU_SPDX_SCHEMA_VERSION}.spdx.json\"}]}")
  set(provenance_path "${CMAKE_BINARY_DIR}/config/laghu-provenance-v1.json")
  file(WRITE "${provenance_path}" "${provenance}")

  set(LAGHU_SPDX_SBOM "${spdx_path}" CACHE INTERNAL "Laghu SPDX SBOM")
  set(LAGHU_CYCLONEDX_SBOM "${cyclonedx_path}" CACHE INTERNAL "Laghu CycloneDX SBOM")
  set(LAGHU_PROVENANCE "${provenance_path}" CACHE INTERNAL "Laghu provenance predicate")
  set(LAGHU_SUPPLY_CHAIN_CREATED "${created}" CACHE INTERNAL "Laghu SPDX creation time")
endfunction()
