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
    .github/workflows/toolchain.yml
    CMakeLists.txt
    CMakePresets.json
    VERSION
    bench/core_foundation.cpp
    bench/event_batch.cpp
    bench/evaluator.cpp
    bench/regression-gates.tsv
    bench/private/laghu/benchmark/internal/metrics.hpp
    bench/private/laghu/benchmark/internal/workload.hpp
    bench/runner.cpp
    cmake/LaghuApiBoundaries.cmake
    cmake/LaghuBenchmarks.cmake
    cmake/ExpectBenchmarkEvaluator.cmake
    cmake/ExpectBenchmarkRunner.cmake
    cmake/LaghuBuildIdentity.cmake
    cmake/LaghuBuildVariants.cmake
    cmake/LaghuCapabilities.cmake
    cmake/LaghuDependencies.cmake
    cmake/LaghuDependencyDag.cmake
    cmake/LaghuFeatures.cmake
    cmake/LaghuHardening.cmake
    cmake/LaghuSanitizers.cmake
    cmake/LaghuSupplyChain.cmake
    cmake/LaghuToolchain.cmake
    cmake/ExpectSupplyChain.cmake
    cmake/ExpectSupplyChainWorkflow.cmake
    cmake/ValidateSupplyChain.cmake
    tests/hardening/probes/clean.cpp
    tests/hardening/probes/fortification.cpp
    tests/benchmarks/workload_counters.cpp
    src/cli/main.cpp
    src/cli/private/laghu/cli/internal/build_manifest.hpp
    src/adapters/contract/laghu/adapters/dependency.hpp
    src/adapters/contract/laghu/adapters/dependency_lifecycle.hpp
    src/adapters/dependency.cpp
    src/adapters/dependency_lifecycle.cpp
    src/adapters/private/laghu/adapters/internal/dependency.hpp
    src/core/clocks.cpp
    src/core/contract.cpp
    src/core/digest.cpp
    src/core/fingerprints.cpp
    src/core/contract/laghu/core/contract.hpp
    src/core/contract/laghu/core/clocks.hpp
    src/core/contract/laghu/core/crypto.hpp
    src/core/contract/laghu/core/deadlines_cancellation.hpp
    src/core/contract/laghu/core/digest.hpp
    src/core/contract/laghu/core/bounded_arena.hpp
    src/core/contract/laghu/core/binary_envelope.hpp
    src/core/contract/laghu/core/handles.hpp
    src/core/contract/laghu/core/identifiers.hpp
    src/core/contract/laghu/core/memory_budget.hpp
    src/core/contract/laghu/core/mapped_regions.hpp
    src/core/contract/laghu/core/views.hpp
    src/core/handles.cpp
    src/core/mapped_regions.cpp
    src/core/private/laghu/core/internal/clock_operations.hpp
    src/core/private/laghu/core/internal/compiler_extensions.hpp
    src/core/private/laghu/core/internal/descriptor_operations.hpp
    src/core/private/laghu/core/internal/fingerprints.hpp
    src/core/private/laghu/core/internal/mapping_operations.hpp
    src/os/io_operations.cpp
    src/os/listener.cpp
    src/os/socket_io.cpp
    src/os/wakeup.cpp
    src/os/contract/laghu/os/listener.hpp
    src/os/contract/laghu/os/socket_io.hpp
    src/os/contract/laghu/os/wakeup.hpp
    src/os/private/laghu/os/internal/listener.hpp
    src/os/private/laghu/os/internal/io_operations.hpp
    src/os/private/laghu/os/internal/socket_io.hpp
    src/os/private/laghu/os/internal/wakeup.hpp
    src/runtime/contract/laghu/runtime/event_backend.hpp
    src/runtime/contract/laghu/runtime/event_batch.hpp
    src/runtime/contract/laghu/runtime/worker_wakeup.hpp
    src/runtime/event_backend.cpp
    src/runtime/worker_wakeup.cpp
    scripts/benchmark
    scripts/benchmark-evaluate
    tests/warnings/suppressions.tsv
    tests/sanitizers/suppressions.tsv)
  list(FIND LAGHU_EFFECTIVE_FEATURES tls tls_feature_index)
  if(NOT tls_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/crypto_provider.hpp
      src/adapters/crypto_provider.cpp
      src/adapters/entropy.cpp
      src/adapters/private/laghu/adapters/internal/entropy.hpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES idna idna_feature_index)
  if(NOT idna_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/idna.hpp
      src/adapters/idna.cpp
      src/adapters/private/laghu/adapters/internal/idna.hpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES password_auth password_auth_feature_index)
  if(NOT password_auth_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/password_auth.hpp
      src/adapters/password_auth.cpp
      src/adapters/private/laghu/adapters/internal/password_auth.hpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES structured_data structured_data_feature_index)
  if(NOT structured_data_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/structured_data.hpp
      src/adapters/structured_data.cpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES http2 http2_feature_index)
  if(NOT http2_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/http2.hpp
      src/adapters/http2.cpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES http3 http3_feature_index)
  if(NOT http3_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/http3.hpp
      src/adapters/contract/laghu/adapters/native_memory.hpp
      src/adapters/contract/laghu/adapters/quic.hpp
      src/adapters/http3.cpp
      src/adapters/quic.cpp
      src/adapters/private/laghu/adapters/internal/arena_memory.hpp)
  endif()
  set(codec_common_added OFF)
  foreach(codec_feature IN ITEMS compression_zlib compression_brotli compression_zstd)
    list(FIND LAGHU_EFFECTIVE_FEATURES "${codec_feature}" codec_feature_index)
    if(NOT codec_feature_index EQUAL -1)
      if(NOT codec_common_added)
        list(APPEND inputs
          src/adapters/contract/laghu/adapters/codecs.hpp
          src/adapters/contract/laghu/adapters/native_memory.hpp
          src/adapters/private/laghu/adapters/internal/arena_memory.hpp
          src/adapters/private/laghu/adapters/internal/codecs.hpp
          src/adapters/codecs.cpp
          bench/codecs.cpp
          cmake/ExpectCodecBenchmark.cmake
          cmake/ExpectCodecIsolation.cmake
          tests/adapters/codecs.cpp
          tests/adapters/codecs_contract.cpp)
        set(codec_common_added ON)
      endif()
      if(codec_feature STREQUAL compression_zlib)
        list(APPEND inputs src/adapters/codec_zlib_ng.cpp)
      elseif(codec_feature STREQUAL compression_brotli)
        list(APPEND inputs src/adapters/codec_brotli.cpp)
      else()
        list(APPEND inputs src/adapters/codec_zstd.cpp)
      endif()
    endif()
  endforeach()
  list(FIND LAGHU_EFFECTIVE_FEATURES async_dns async_dns_feature_index)
  if(NOT async_dns_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/dns.hpp
      src/adapters/dns.cpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES regex regex_feature_index)
  if(NOT regex_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/native_memory.hpp
      src/adapters/contract/laghu/adapters/regex.hpp
      src/adapters/private/laghu/adapters/internal/arena_memory.hpp
      src/adapters/regex.cpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES geoip geoip_feature_index)
  if(NOT geoip_feature_index EQUAL -1)
    list(APPEND inputs
      src/adapters/contract/laghu/adapters/geoip.hpp
      src/adapters/geoip.cpp
      tests/adapters/geoip.cpp
      tests/adapters/geoip_contract.cpp)
  endif()
  list(FIND LAGHU_EFFECTIVE_FEATURES otlp otlp_feature_index)
  if(NOT otlp_feature_index EQUAL -1)
    list(APPEND inputs
      cmake/LaghuOtlp.cmake
      src/adapters/contract/laghu/adapters/otlp.hpp
      src/adapters/otlp.cpp
      tests/adapters/otlp.cpp
      tests/adapters/otlp_contract.cpp)
  endif()
  list(REMOVE_DUPLICATES inputs)
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
  list(FIND dependencies protobuf_c protobuf_c_index)
  if(LAGHU_DEPENDENCY_SOURCE STREQUAL SYSTEM AND NOT protobuf_c_index EQUAL -1)
    list(APPEND dependencies protobuf_c_host)
  endif()
  list(SORT dependencies)
  set(entries)
  foreach(id IN LISTS dependencies)
    if(id STREQUAL protobuf_c_host)
      set(registry_id protobuf_c)
    else()
      set(registry_id "${id}")
    endif()
    laghu_dependency_property("${registry_id}" ARCHIVE_URL archive_url)
    laghu_dependency_property("${registry_id}" ARCHIVE_SHA256 archive_sha256)
    laghu_dependency_property("${registry_id}" LICENSE_EXPRESSION license_expression)
    laghu_dependency_property("${registry_id}" VENDORED_VERSION vendored_version)
    if(id STREQUAL protobuf_c_host)
      set(selected_version "${vendored_version}")
    else()
      get_property(selected_version GLOBAL PROPERTY
        "LAGHU_DEPENDENCY_SELECTED_VERSION_${registry_id}")
      if("${selected_version}" STREQUAL "" OR "${selected_version}" MATCHES "-NOTFOUND$")
        set(selected_version "${vendored_version}")
      endif()
    endif()
    laghu_dependency_property("${registry_id}" SOURCE_ONLY source_only)
    if(id STREQUAL protobuf_c_host)
      set(identity_source VENDORED)
      set(identity_linkage HOST_TOOL)
      set(identity_url "${archive_url}")
      set(identity_sha256 "${archive_sha256}")
      set(identity_verification verified-archive)
    elseif(source_only)
      set(identity_source VENDORED)
      set(identity_linkage SOURCE_ONLY)
      set(identity_url "${archive_url}")
      set(identity_sha256 "${archive_sha256}")
      set(identity_verification verified-archive)
    else()
      set(identity_source "${LAGHU_DEPENDENCY_SOURCE}")
      set(identity_linkage "${LAGHU_DEPENDENCY_LINK_MODE}")
      if(identity_source STREQUAL SYSTEM)
        set(identity_url "")
        set(identity_sha256 "")
        set(identity_verification system-package-unverified)
      else()
        set(identity_url "${archive_url}")
        set(identity_sha256 "${archive_sha256}")
        set(identity_verification verified-archive)
      endif()
    endif()
    list(APPEND entries
      "{\"provider\":\"${id}\",\"source\":\"${identity_source}\",\"version\":\"${selected_version}\",\"linkage\":\"${identity_linkage}\",\"url\":\"${identity_url}\",\"sha256\":\"${identity_sha256}\",\"license\":\"${license_expression}\",\"verification\":\"${identity_verification}\"}")
  endforeach()
  list(JOIN entries "," rendered_entries)
  set(${output} "[${rendered_entries}]" PARENT_SCOPE)
  set(${output_names} "${dependencies}" PARENT_SCOPE)
endfunction()

function(laghu_build_identity_verbose output build_id compiler requested effective dependencies dependencies_json)
  set(lines
    "build_id=${build_id}"
    "compiler.executable=${compiler}"
    "compiler.id=${CMAKE_CXX_COMPILER_ID}"
    "compiler.version=${CMAKE_CXX_COMPILER_VERSION}"
    "product.name=laghu"
    "product.version=${LAGHU_PRODUCT_VERSION}"
    "profile=${LAGHU_BUILD_PROFILE}"
    "hardening=${LAGHU_HARDENING_METADATA_JSON}"
    "sanitizer.profile=${LAGHU_SANITIZER_PROFILE}"
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
  set(dependency_index 0)
  foreach(id IN LISTS dependencies)
    foreach(field IN ITEMS linkage provider sha256 source url verification version)
      string(JSON identity_${field} GET "${dependencies_json}" ${dependency_index} ${field})
    endforeach()
    list(APPEND lines
      "dependency.${id}.linkage=${identity_linkage}"
      "dependency.${id}.provider=${identity_provider}"
      "dependency.${id}.sha256=${identity_sha256}"
      "dependency.${id}.source=${identity_source}"
      "dependency.${id}.url=${identity_url}"
      "dependency.${id}.verification=${identity_verification}"
      "dependency.${id}.version=${identity_version}")
    math(EXPR dependency_index "${dependency_index} + 1")
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
    "{\"schema_version\":\"${LAGHU_BUILD_MANIFEST_SCHEMA_VERSION}\",\"product\":{\"name\":\"laghu\",\"version\":\"${product_version}\"},\"build_inputs\":${inputs_json},\"compiler\":{\"executable\":\"${compiler}\",\"id\":\"${CMAKE_CXX_COMPILER_ID}\",\"version\":\"${CMAKE_CXX_COMPILER_VERSION}\"},\"standard_library\":{\"id\":\"${LAGHU_STANDARD_LIBRARY_ID}\",\"version\":\"${LAGHU_STANDARD_LIBRARY_VERSION}\"},\"target\":{\"os\":\"${CMAKE_SYSTEM_NAME}\",\"architecture\":\"${CMAKE_SYSTEM_PROCESSOR}\"},\"profile\":\"${LAGHU_BUILD_PROFILE}\",\"sanitizer_profile\":\"${LAGHU_SANITIZER_PROFILE}\",\"hardening\":${LAGHU_HARDENING_METADATA_JSON},\"features\":{\"requested\":${requested_json},\"effective\":${effective_json}},\"dependencies\":${dependencies_json}}")
  string(SHA256 build_id "${preimage}")
  set(manifest "${preimage}")
  string(REGEX REPLACE "}$" ",\"build_id\":\"${build_id}\"}" manifest "${manifest}")

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config" "${CMAKE_BINARY_DIR}/generated/laghu")
  set(preimage_path "${CMAKE_BINARY_DIR}/config/laghu-build-identity-preimage-v1.json")
  set(manifest_path "${CMAKE_BINARY_DIR}/config/laghu-build-manifest-v1.json")
  file(WRITE "${preimage_path}" "${preimage}\n")
  file(WRITE "${manifest_path}" "${manifest}\n")
  laghu_build_identity_verbose(verbose "${build_id}" "${compiler}" "${requested}"
    "${effective}" "${dependency_names}" "${dependencies_json}")
  set(generated_source "${CMAKE_BINARY_DIR}/generated/laghu/build_manifest.cpp")
  file(WRITE "${generated_source}"
"// SPDX-License-Identifier: AGPL-3.0-only\n#include <laghu/cli/internal/build_manifest.hpp>\n\nnamespace laghu::cli::internal {\nconst char* build_manifest() noexcept { return R\"laghu(${manifest})laghu\"; }\nconst char* build_manifest_verbose() noexcept { return R\"laghu(${verbose})laghu\"; }\n}  // namespace laghu::cli::internal\n")
  set(LAGHU_BUILD_MANIFEST "${manifest_path}" CACHE INTERNAL "Laghu canonical build manifest")
  set(LAGHU_BUILD_IDENTITY_PREIMAGE "${preimage_path}" CACHE INTERNAL "Laghu build identity preimage")
  set(LAGHU_BUILD_ID "${build_id}" CACHE INTERNAL "Laghu stable build identifier")
  set(LAGHU_BUILD_MANIFEST_SOURCE "${generated_source}" CACHE INTERNAL "Laghu generated build manifest source")
  set(LAGHU_BUILD_IDENTITY_FEATURES_JSON "${effective_json}" CACHE INTERNAL
    "Laghu effective feature identity JSON" FORCE)
  set(LAGHU_BUILD_IDENTITY_DEPENDENCIES_JSON "${dependencies_json}" CACHE INTERNAL
    "Laghu effective dependency identity JSON" FORCE)
endfunction()
