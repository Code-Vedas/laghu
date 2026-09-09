# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_BUILD_MANIFEST_SCHEMA_VERSION laghu-build-manifest-v1)

function(laghu_build_identity_json_array output)
  if(ARGN)
    list(JOIN ARGN "\",\"" members)
    set(${output} "[\"${members}\"]" PARENT_SCOPE)
  else()
    set(${output} "[]" PARENT_SCOPE)
  endif()
endfunction()

function(laghu_build_identity_relative_compiler output)
  get_filename_component(compiler_name "${CMAKE_CXX_COMPILER}" NAME)
  if(compiler_name STREQUAL "")
    message(FATAL_ERROR "Laghu build identity failed: rule=compiler_name_missing")
  endif()
  set(${output} "${compiler_name}" PARENT_SCOPE)
endfunction()

function(laghu_build_identity_input_hashes output)
  set(inputs
    CMakeLists.txt
    CMakePresets.json
    VERSION
    cmake/LaghuApiBoundaries.cmake
    cmake/LaghuBuildIdentity.cmake
    cmake/LaghuBuildVariants.cmake
    cmake/LaghuCapabilities.cmake
    cmake/LaghuDependencies.cmake
    cmake/LaghuDependencyDag.cmake
    cmake/LaghuFeatures.cmake
    cmake/LaghuToolchain.cmake
    src/cli/main.cpp
    src/cli/private/laghu/cli/internal/build_manifest.hpp
    src/core/contract.cpp
    src/core/contract/laghu/core/contract.hpp
    src/core/contract/laghu/core/bounded_arena.hpp
    src/core/contract/laghu/core/handles.hpp
    src/core/contract/laghu/core/identifiers.hpp
    src/core/contract/laghu/core/memory_budget.hpp
    src/core/handles.cpp
    src/core/private/laghu/core/internal/compiler_extensions.hpp
    src/core/private/laghu/core/internal/descriptor_operations.hpp
    tests/warnings/suppressions.tsv)
  set(entries)
  foreach(input IN LISTS inputs)
    set(path "${CMAKE_SOURCE_DIR}/${input}")
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "Laghu build identity failed: rule=declared_input_missing path=${input}")
    endif()
    file(SHA256 "${path}" sha256)
    list(APPEND entries "{\"path\":\"${input}\",\"sha256\":\"${sha256}\"}")
  endforeach()
  list(JOIN entries "," rendered_entries)
  set(${output} "[${rendered_entries}]" PARENT_SCOPE)
endfunction()

function(laghu_build_identity_dependencies output output_names)
  set(dependencies ${LAGHU_ACTIVE_DEPENDENCIES})
  list(SORT dependencies)
  set(entries)
  foreach(id IN LISTS dependencies)
    laghu_dependency_property("${id}" ARCHIVE_URL archive_url)
    laghu_dependency_property("${id}" ARCHIVE_SHA256 archive_sha256)
    laghu_dependency_property("${id}" VENDORED_VERSION vendored_version)
    get_property(selected_version GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}")
    if(selected_version STREQUAL "")
      set(selected_version "${vendored_version}")
    endif()
    list(APPEND entries
      "{\"provider\":\"${id}\",\"source\":\"${LAGHU_DEPENDENCY_SOURCE}\",\"version\":\"${selected_version}\",\"linkage\":\"${LAGHU_DEPENDENCY_LINK_MODE}\",\"url\":\"${archive_url}\",\"sha256\":\"${archive_sha256}\"}")
  endforeach()
  list(JOIN entries "," rendered_entries)
  set(${output} "[${rendered_entries}]" PARENT_SCOPE)
  set(${output_names} "${dependencies}" PARENT_SCOPE)
endfunction()

function(laghu_build_identity_verbose output build_id compiler requested effective dependencies)
  set(lines
    "build_id=${build_id}"
    "compiler.executable=${compiler}"
    "compiler.id=${CMAKE_CXX_COMPILER_ID}"
    "compiler.version=${CMAKE_CXX_COMPILER_VERSION}"
    "product.name=laghu"
    "product.version=${LAGHU_PRODUCT_VERSION}"
    "profile=${LAGHU_BUILD_PROFILE}"
    "schema_version=${LAGHU_BUILD_MANIFEST_SCHEMA_VERSION}"
    "standard_library.id=${LAGHU_STANDARD_LIBRARY_ID}"
    "standard_library.version=${LAGHU_STANDARD_LIBRARY_VERSION}"
    "target.architecture=${CMAKE_SYSTEM_PROCESSOR}"
    "target.os=${CMAKE_SYSTEM_NAME}")
  list(JOIN requested "," requested_text)
  list(JOIN effective "," effective_text)
  list(JOIN dependencies "," dependencies_text)
  list(APPEND lines
    "dependencies=${dependencies_text}"
    "features.effective=${effective_text}"
    "features.requested=${requested_text}")
  foreach(id IN LISTS dependencies)
    laghu_dependency_property("${id}" ARCHIVE_URL archive_url)
    laghu_dependency_property("${id}" ARCHIVE_SHA256 archive_sha256)
    laghu_dependency_property("${id}" VENDORED_VERSION vendored_version)
    get_property(selected_version GLOBAL PROPERTY "LAGHU_DEPENDENCY_SELECTED_VERSION_${id}")
    if(selected_version STREQUAL "")
      set(selected_version "${vendored_version}")
    endif()
    list(APPEND lines
      "dependency.${id}.linkage=${LAGHU_DEPENDENCY_LINK_MODE}"
      "dependency.${id}.provider=${id}"
      "dependency.${id}.sha256=${archive_sha256}"
      "dependency.${id}.source=${LAGHU_DEPENDENCY_SOURCE}"
      "dependency.${id}.url=${archive_url}"
      "dependency.${id}.version=${selected_version}")
  endforeach()
  list(SORT lines)
  list(JOIN lines "\n" rendered_lines)
  set(${output} "${rendered_lines}\n" PARENT_SCOPE)
endfunction()

function(laghu_configure_build_identity)
  set(version_file "${CMAKE_SOURCE_DIR}/VERSION")
  if(NOT EXISTS "${version_file}")
    message(FATAL_ERROR "Laghu build identity failed: rule=version_file_missing")
  endif()
  file(READ "${version_file}" product_version)
  string(STRIP "${product_version}" product_version)
  if(NOT product_version STREQUAL "0.0.1")
    message(FATAL_ERROR "Laghu build identity failed: rule=product_version_invalid value=${product_version}")
  endif()
  set(LAGHU_PRODUCT_VERSION "${product_version}" CACHE INTERNAL "Laghu product version")

  laghu_build_identity_relative_compiler(compiler)
  laghu_build_identity_input_hashes(inputs_json)
  set(requested ${LAGHU_BUILD_REQUESTED_FEATURES})
  set(effective ${LAGHU_BUILD_EFFECTIVE_FEATURES})
  list(SORT requested)
  list(SORT effective)
  laghu_build_identity_json_array(requested_json ${requested})
  laghu_build_identity_json_array(effective_json ${effective})
  laghu_build_identity_dependencies(dependencies_json dependency_names)

  set(preimage
    "{\"schema_version\":\"${LAGHU_BUILD_MANIFEST_SCHEMA_VERSION}\",\"product\":{\"name\":\"laghu\",\"version\":\"${product_version}\"},\"build_inputs\":${inputs_json},\"compiler\":{\"executable\":\"${compiler}\",\"id\":\"${CMAKE_CXX_COMPILER_ID}\",\"version\":\"${CMAKE_CXX_COMPILER_VERSION}\"},\"standard_library\":{\"id\":\"${LAGHU_STANDARD_LIBRARY_ID}\",\"version\":\"${LAGHU_STANDARD_LIBRARY_VERSION}\"},\"target\":{\"os\":\"${CMAKE_SYSTEM_NAME}\",\"architecture\":\"${CMAKE_SYSTEM_PROCESSOR}\"},\"profile\":\"${LAGHU_BUILD_PROFILE}\",\"features\":{\"requested\":${requested_json},\"effective\":${effective_json}},\"dependencies\":${dependencies_json}}")
  string(SHA256 build_id "${preimage}")
  set(manifest "${preimage}")
  string(REGEX REPLACE "}$" ",\"build_id\":\"${build_id}\"}" manifest "${manifest}")

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config" "${CMAKE_BINARY_DIR}/generated/laghu")
  set(preimage_path "${CMAKE_BINARY_DIR}/config/laghu-build-identity-preimage-v1.json")
  set(manifest_path "${CMAKE_BINARY_DIR}/config/laghu-build-manifest-v1.json")
  file(WRITE "${preimage_path}" "${preimage}\n")
  file(WRITE "${manifest_path}" "${manifest}\n")
  laghu_build_identity_verbose(verbose "${build_id}" "${compiler}" "${requested}" "${effective}" "${dependency_names}")
  set(generated_source "${CMAKE_BINARY_DIR}/generated/laghu/build_manifest.cpp")
  file(WRITE "${generated_source}"
"// SPDX-License-Identifier: AGPL-3.0-only\n#include <laghu/cli/internal/build_manifest.hpp>\n\nnamespace laghu::cli::internal {\nconst char* build_manifest() noexcept { return R\"laghu(${manifest})laghu\"; }\nconst char* build_manifest_verbose() noexcept { return R\"laghu(${verbose})laghu\"; }\n}  // namespace laghu::cli::internal\n")
  set(LAGHU_BUILD_MANIFEST "${manifest_path}" CACHE INTERNAL "Laghu canonical build manifest")
  set(LAGHU_BUILD_IDENTITY_PREIMAGE "${preimage_path}" CACHE INTERNAL "Laghu build identity preimage")
  set(LAGHU_BUILD_ID "${build_id}" CACHE INTERNAL "Laghu stable build identifier")
  set(LAGHU_BUILD_MANIFEST_SOURCE "${generated_source}" CACHE INTERNAL "Laghu generated build manifest source")
endfunction()
