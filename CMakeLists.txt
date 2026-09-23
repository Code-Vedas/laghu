# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.28)

project(laghu LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "Laghu requires an out-of-tree build. Configure with -S <source> -B <build>.")
endif()

# Laghu does not use C++ modules at this milestone.  Disable CMake's automatic
# module dependency scanning so a supported Clang compiler does not require a
# separate clang-scan-deps executable merely to compile ordinary C++23 probes.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR
    "Laghu requires the single-config Ninja generator; generator=${CMAKE_GENERATOR}. Configure with -G Ninja.")
endif()

include(CTest)
include(cmake/LaghuCapabilities.cmake)
include(cmake/LaghuDependencyDag.cmake)
include(cmake/LaghuApiBoundaries.cmake)
include(cmake/LaghuBenchmarks.cmake)
include(cmake/LaghuDependencies.cmake)
include(cmake/LaghuFeatures.cmake)
include(cmake/LaghuFuzzing.cmake)
include(cmake/LaghuHardening.cmake)
include(cmake/LaghuOtlp.cmake)
include(cmake/LaghuSanitizers.cmake)
include(cmake/LaghuStaticAnalysis.cmake)
include(cmake/LaghuBuildVariants.cmake)
include(cmake/LaghuBuildIdentity.cmake)
include(cmake/LaghuSupplyChain.cmake)
include(cmake/LaghuTestRegistration.cmake)
include(cmake/LaghuToolchain.cmake)

laghu_configure_toolchain()
laghu_configure_sanitizer_profile()
laghu_configure_hardening()
laghu_write_metadata()
laghu_configure_static_analysis()
laghu_configure_fuzzing()
laghu_configure_build_variant()
list(FIND LAGHU_EFFECTIVE_FEATURES otlp laghu_otlp_feature_index)
if(NOT laghu_otlp_feature_index EQUAL -1)
  enable_language(C)
endif()
laghu_configure_dependency_registry()
laghu_configure_dependency_modes()
laghu_configure_capability_header()
laghu_configure_build_identity()
laghu_configure_supply_chain()
laghu_configure_benchmarks()

add_library(laghu_core STATIC
  src/core/clocks.cpp
  src/core/contract.cpp
  src/core/digest.cpp
  src/core/fingerprints.cpp
  src/core/handles.cpp
  src/core/mapped_regions.cpp)
laghu_apply_first_party_contract(laghu_core)
target_link_libraries(laghu_core PRIVATE laghu_feature_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(src/core/mapped_regions.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()

find_package(Threads REQUIRED)

# Test support is a first-party, build-local library. It is intentionally not
# registered as a subsystem and is never installed or linked by runtime targets.
add_library(laghu_test_support STATIC tests/support/laghu_test_support.cpp)
laghu_apply_first_party_contract(laghu_test_support)
target_include_directories(laghu_test_support PUBLIC "${CMAKE_SOURCE_DIR}/tests/support")

# Fault controls are build-local test support. Its complete interface is
# deliberately private to Laghu's test targets because the wrapper contracts
# name internal core and OS seams.
add_library(laghu_test_faults STATIC tests/support/laghu_test_faults.cpp)
laghu_apply_first_party_contract(laghu_test_faults)
target_include_directories(laghu_test_faults PUBLIC
  "${CMAKE_SOURCE_DIR}/tests/support"
  "${CMAKE_SOURCE_DIR}/src/core/contract"
  "${CMAKE_SOURCE_DIR}/src/core/private"
  "${CMAKE_SOURCE_DIR}/src/os/private")

add_executable(laghu_test_support_fixtures_test tests/support/fixtures.cpp)
laghu_apply_first_party_contract(laghu_test_support_fixtures_test)
target_link_libraries(laghu_test_support_fixtures_test PRIVATE laghu_test_support Threads::Threads)
laghu_add_native_test(laghu.test_support.fixtures laghu_test_support_fixtures_test)

add_executable(laghu_test_support_runner_contract_test tests/support/runner_contract.cpp)
laghu_apply_first_party_contract(laghu_test_support_runner_contract_test)
target_link_libraries(laghu_test_support_runner_contract_test PRIVATE laghu_test_support)
laghu_add_native_test(laghu.test_support.runner_contract laghu_test_support_runner_contract_test)

add_executable(laghu_test_fault_injection_test tests/support/fault_injection.cpp)
laghu_apply_first_party_contract(laghu_test_fault_injection_test)
laghu_configure_api_consumer(laghu_test_fault_injection_test os)
target_link_libraries(laghu_test_fault_injection_test PRIVATE
  laghu_test_support laghu_test_faults laghu_core laghu_os)
laghu_add_native_test(laghu.test_support.fault_injection laghu_test_fault_injection_test)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # POSIX requires fixed C buffers for sockaddr, mkdtemp, and directory APIs.
  # Keep the narrow diagnostic waiver confined to this non-installed test support.
  set_source_files_properties(
    tests/support/laghu_test_support.cpp
    tests/support/fixtures.cpp
    tests/support/fault_injection.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()

laghu_add_benchmark_targets()

laghu_declare_subsystem_graph()
laghu_configure_api_boundaries()
laghu_apply_first_party_contract(laghu_os)
laghu_configure_api_consumer(laghu_os os)
target_include_directories(laghu_os PRIVATE "${CMAKE_SOURCE_DIR}/src/os/private")
laghu_apply_first_party_contract(laghu_runtime)
laghu_configure_api_consumer(laghu_runtime runtime)

# Deterministic time and entropy are test-only controls. They are kept in a
# separate non-installed target so production entropy remains OS-provided.
add_library(laghu_test_time_entropy STATIC tests/support/laghu_test_time_entropy.cpp)
laghu_apply_first_party_contract(laghu_test_time_entropy)
target_include_directories(laghu_test_time_entropy PUBLIC
  "${CMAKE_SOURCE_DIR}/tests/support"
  "${CMAKE_SOURCE_DIR}/src/core/contract"
  "${CMAKE_SOURCE_DIR}/src/adapters/private")

add_executable(laghu_test_time_entropy_test tests/support/time_entropy.cpp)
laghu_apply_first_party_contract(laghu_test_time_entropy_test)
laghu_configure_api_consumer(laghu_test_time_entropy_test core)
target_include_directories(laghu_test_time_entropy_test PRIVATE
  "${CMAKE_SOURCE_DIR}/src/core/private")
target_link_libraries(laghu_test_time_entropy_test PRIVATE
  laghu_test_support laghu_test_time_entropy laghu_core)
laghu_add_native_test(laghu.test_support.time_entropy laghu_test_time_entropy_test)

# The lifecycle registry belongs to adapters but is shared by the minimal
# lifecycle fixture and the optional crypto adapter. An unregistered object
# target keeps that implementation out of the subsystem DAG while preserving
# the first-party compile contract for the source itself.
add_library(laghu_dependency_lifecycle_impl OBJECT src/adapters/dependency_lifecycle.cpp)
laghu_apply_first_party_contract(laghu_dependency_lifecycle_impl)
laghu_configure_api_consumer(laghu_dependency_lifecycle_impl adapters)

# Dependency-error normalization is shared by optional adapters while its
# implementation remains private and unregistered in the subsystem DAG.
add_library(laghu_dependency_logging_impl OBJECT src/adapters/dependency.cpp)
laghu_apply_first_party_contract(laghu_dependency_logging_impl)
laghu_configure_api_consumer(laghu_dependency_logging_impl adapters)
target_include_directories(laghu_dependency_logging_impl PRIVATE
  "${CMAKE_SOURCE_DIR}/src/adapters/private")

list(FIND LAGHU_EFFECTIVE_FEATURES tls laghu_tls_feature_index)
if(NOT laghu_tls_feature_index EQUAL -1)
  add_library(laghu_crypto STATIC
    src/adapters/crypto_provider.cpp
    src/adapters/entropy.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>
    $<TARGET_OBJECTS:laghu_dependency_lifecycle_impl>)
  laghu_apply_first_party_contract(laghu_crypto)
  laghu_configure_api_consumer(laghu_crypto adapters)
  target_include_directories(laghu_crypto PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_crypto adapters)
  laghu_link_subsystems(laghu_crypto laghu_core)
  target_link_libraries(laghu_crypto PRIVATE laghu_feature_tls)
  if(LAGHU_TLS_PROVIDER STREQUAL "OPENSSL")
    target_compile_definitions(laghu_crypto PRIVATE LAGHU_CRYPTO_DEPENDENCY_OPENSSL=1)
  elseif(LAGHU_TLS_PROVIDER STREQUAL "LIBRESSL")
    target_compile_definitions(laghu_crypto PRIVATE LAGHU_CRYPTO_DEPENDENCY_LIBRESSL=1)
  else()
    message(FATAL_ERROR "Laghu dependency logging failed: tls_provider=${LAGHU_TLS_PROVIDER}")
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES idna laghu_idna_feature_index)
if(NOT laghu_idna_feature_index EQUAL -1)
  add_library(laghu_idna STATIC
    src/adapters/idna.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_idna)
  laghu_configure_api_consumer(laghu_idna adapters)
  target_include_directories(laghu_idna PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_idna adapters)
  laghu_link_subsystems(laghu_idna laghu_core)
  target_link_libraries(laghu_idna PRIVATE laghu_feature_idna)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # libidn2 owns the returned C string; this adapter bounds that native
    # pointer before it crosses into the Laghu-owned hostname value.
    set_source_files_properties(src/adapters/idna.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES password_auth laghu_password_auth_feature_index)
if(NOT laghu_password_auth_feature_index EQUAL -1)
  add_library(laghu_password_auth STATIC
    src/adapters/password_auth.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_password_auth)
  laghu_configure_api_consumer(laghu_password_auth adapters)
  target_include_directories(laghu_password_auth PRIVATE
    "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_password_auth adapters)
  laghu_link_subsystems(laghu_password_auth laghu_core)
  target_link_libraries(laghu_password_auth PRIVATE laghu_feature_password_auth)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # libxcrypt's C result has no span form; this adapter bounds it before the
    # constant-time comparison with the caller's encoded hash.
    set_source_files_properties(src/adapters/password_auth.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES structured_data laghu_structured_data_feature_index)
if(NOT laghu_structured_data_feature_index EQUAL -1)
  add_library(laghu_structured_data STATIC
    src/adapters/structured_data.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_structured_data)
  laghu_configure_api_consumer(laghu_structured_data adapters)
  laghu_register_subsystem_target(laghu_structured_data adapters)
  laghu_link_subsystems(laghu_structured_data laghu_core)
  target_link_libraries(laghu_structured_data PRIVATE laghu_feature_structured_data)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # yyjson is a C header with inline implementation details. Keep the narrow
    # waiver on this adapter translation unit; third-party code receives none
    # of Laghu's warning policy and no Laghu API exposes its declarations.
    set_source_files_properties(src/adapters/structured_data.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant")
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES http2 laghu_http2_feature_index)
if(NOT laghu_http2_feature_index EQUAL -1)
  add_library(laghu_http2 STATIC
    src/adapters/http2.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_http2)
  laghu_configure_api_consumer(laghu_http2 adapters)
  laghu_register_subsystem_target(laghu_http2 adapters)
  laghu_link_subsystems(laghu_http2 laghu_core)
  target_link_libraries(laghu_http2 PRIVATE laghu_feature_http2)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/adapters/http2.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant")
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES http3 laghu_http3_feature_index)
if(NOT laghu_http3_feature_index EQUAL -1)
  add_library(laghu_quic STATIC
    src/adapters/quic.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_quic)
  laghu_configure_api_consumer(laghu_quic adapters)
  target_include_directories(laghu_quic PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_quic adapters)
  laghu_link_subsystems(laghu_quic laghu_core)
  target_link_libraries(laghu_quic PRIVATE laghu_feature_http3)

  add_library(laghu_http3 STATIC
    src/adapters/http3.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_http3)
  laghu_configure_api_consumer(laghu_http3 adapters)
  target_include_directories(laghu_http3 PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_http3 adapters)
  laghu_link_subsystems(laghu_http3 laghu_core)
  target_link_libraries(laghu_http3 PRIVATE laghu_feature_http3)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/adapters/quic.cpp src/adapters/http3.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant")
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES async_dns laghu_async_dns_feature_index)
if(NOT laghu_async_dns_feature_index EQUAL -1)
  add_library(laghu_dns STATIC
    src/adapters/dns.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_dns)
  laghu_configure_api_consumer(laghu_dns adapters)
  laghu_register_subsystem_target(laghu_dns adapters)
  laghu_link_subsystems(laghu_dns laghu_core)
  target_link_libraries(laghu_dns PRIVATE laghu_feature_async_dns)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/adapters/dns.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant;-Wno-deprecated-declarations")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/adapters/dns.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-deprecated-declarations)
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES regex laghu_regex_feature_index)
if(NOT laghu_regex_feature_index EQUAL -1)
  add_library(laghu_regex STATIC
    src/adapters/regex.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_regex)
  laghu_configure_api_consumer(laghu_regex adapters)
  target_include_directories(laghu_regex PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_regex adapters)
  laghu_link_subsystems(laghu_regex laghu_core)
  target_link_libraries(laghu_regex PRIVATE laghu_feature_regex)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/adapters/regex.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant")
  endif()
endif()

list(FIND LAGHU_EFFECTIVE_FEATURES geoip laghu_geoip_feature_index)
if(NOT laghu_geoip_feature_index EQUAL -1)
  add_library(laghu_geoip STATIC
    src/adapters/geoip.cpp
    $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
  laghu_apply_first_party_contract(laghu_geoip)
  laghu_configure_api_consumer(laghu_geoip adapters)
  laghu_register_subsystem_target(laghu_geoip adapters)
  laghu_link_subsystems(laghu_geoip laghu_core)
  target_link_libraries(laghu_geoip PRIVATE laghu_feature_geoip)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(src/adapters/geoip.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant;-Wno-undef")
  endif()
endif()

if(NOT laghu_otlp_feature_index EQUAL -1)
  laghu_configure_otlp_schema(laghu_otlp_schema_target laghu_otlp_generated)
  add_library(laghu_otlp STATIC src/adapters/otlp.cpp)
  laghu_apply_first_party_contract(laghu_otlp)
  laghu_configure_api_consumer(laghu_otlp adapters)
  laghu_register_subsystem_target(laghu_otlp adapters)
  laghu_link_subsystems(laghu_otlp laghu_core)
  target_link_libraries(laghu_otlp PRIVATE laghu_feature_otlp laghu_otlp_schema)
endif()

set(laghu_codec_targets)
foreach(laghu_codec_feature IN ITEMS
    compression_zlib compression_brotli compression_zstd)
  list(FIND LAGHU_EFFECTIVE_FEATURES "${laghu_codec_feature}" laghu_codec_feature_index)
  if(NOT laghu_codec_feature_index EQUAL -1)
    list(APPEND laghu_codec_targets "${laghu_codec_feature}")
  endif()
endforeach()
if(laghu_codec_targets)
  add_library(laghu_codecs STATIC src/adapters/codecs.cpp)
  laghu_apply_first_party_contract(laghu_codecs)
  laghu_configure_api_consumer(laghu_codecs adapters)
  target_include_directories(laghu_codecs PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_codecs adapters)
  laghu_link_subsystems(laghu_codecs laghu_core)

  foreach(laghu_codec_feature IN LISTS laghu_codec_targets)
    if(laghu_codec_feature STREQUAL compression_zlib)
      set(laghu_codec_suffix zlib_ng)
    elseif(laghu_codec_feature STREQUAL compression_brotli)
      set(laghu_codec_suffix brotli)
    else()
      set(laghu_codec_suffix zstd)
    endif()
    add_library("laghu_codec_${laghu_codec_suffix}" STATIC
      "src/adapters/codec_${laghu_codec_suffix}.cpp"
      $<TARGET_OBJECTS:laghu_dependency_logging_impl>)
    laghu_apply_first_party_contract("laghu_codec_${laghu_codec_suffix}")
    laghu_configure_api_consumer("laghu_codec_${laghu_codec_suffix}" adapters)
    target_include_directories("laghu_codec_${laghu_codec_suffix}" PRIVATE
      "${CMAKE_SOURCE_DIR}/src/adapters/private")
    laghu_register_subsystem_target("laghu_codec_${laghu_codec_suffix}" adapters)
    laghu_link_subsystems("laghu_codec_${laghu_codec_suffix}" laghu_core)
    target_link_libraries("laghu_codec_${laghu_codec_suffix}" PRIVATE
      laghu_codecs "laghu_feature_${laghu_codec_feature}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set_source_files_properties("src/adapters/codec_${laghu_codec_suffix}.cpp" PROPERTIES
        COMPILE_OPTIONS "-Wno-old-style-cast;-Wno-unsafe-buffer-usage;-Wno-zero-as-null-pointer-constant")
    endif()
  endforeach()
endif()

add_executable(laghu_core_smoke src/core/smoke.cpp)
laghu_apply_first_party_contract(laghu_core_smoke)
laghu_configure_api_consumer(laghu_core_smoke core)
target_link_libraries(laghu_core_smoke PRIVATE laghu_core)

add_executable(laghu_core_result_errors_test tests/core/result_errors.cpp)
laghu_apply_first_party_contract(laghu_core_result_errors_test)
laghu_configure_api_consumer(laghu_core_result_errors_test core)
target_link_libraries(laghu_core_result_errors_test PRIVATE laghu_core)
target_link_libraries(laghu_core_result_errors_test PRIVATE laghu_test_support)
laghu_add_native_test(laghu.core.result_errors laghu_core_result_errors_test)

add_executable(laghu_core_strong_identifiers_test tests/core/strong_identifiers.cpp)
laghu_apply_first_party_contract(laghu_core_strong_identifiers_test)
laghu_configure_api_consumer(laghu_core_strong_identifiers_test core)
target_link_libraries(laghu_core_strong_identifiers_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.strong_identifiers laghu_core_strong_identifiers_test)

add_executable(laghu_core_checked_arithmetic_test tests/core/checked_arithmetic.cpp)
laghu_apply_first_party_contract(laghu_core_checked_arithmetic_test)
laghu_configure_api_consumer(laghu_core_checked_arithmetic_test core)
target_link_libraries(laghu_core_checked_arithmetic_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.checked_arithmetic laghu_core_checked_arithmetic_test)

add_executable(laghu_core_byte_text_views_test tests/core/byte_text_views.cpp)
laghu_apply_first_party_contract(laghu_core_byte_text_views_test)
laghu_configure_api_consumer(laghu_core_byte_text_views_test core)
target_link_libraries(laghu_core_byte_text_views_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.byte_text_views laghu_core_byte_text_views_test)

add_executable(laghu_core_fd_handles_test tests/core/fd_handles.cpp)
laghu_apply_first_party_contract(laghu_core_fd_handles_test)
laghu_configure_api_consumer(laghu_core_fd_handles_test core)
target_include_directories(laghu_core_fd_handles_test PRIVATE "${CMAKE_SOURCE_DIR}/src/core/private")
target_link_libraries(laghu_core_fd_handles_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.fd_handles laghu_core_fd_handles_test)

add_executable(laghu_core_memory_budgets_test tests/core/memory_budgets.cpp)
laghu_apply_first_party_contract(laghu_core_memory_budgets_test)
laghu_configure_api_consumer(laghu_core_memory_budgets_test core)
target_link_libraries(laghu_core_memory_budgets_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.memory_budgets laghu_core_memory_budgets_test)

add_executable(laghu_core_bounded_arena_test tests/core/bounded_arena.cpp)
laghu_apply_first_party_contract(laghu_core_bounded_arena_test)
laghu_configure_api_consumer(laghu_core_bounded_arena_test core)
target_link_libraries(laghu_core_bounded_arena_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.bounded_arena laghu_core_bounded_arena_test)

add_executable(laghu_core_slab_pools_test tests/core/slab_pools.cpp)
laghu_apply_first_party_contract(laghu_core_slab_pools_test)
laghu_configure_api_consumer(laghu_core_slab_pools_test core)
target_link_libraries(laghu_core_slab_pools_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.slab_pools laghu_core_slab_pools_test)

add_executable(laghu_core_bounded_buffer_test tests/core/bounded_buffers.cpp)
laghu_apply_first_party_contract(laghu_core_bounded_buffer_test)
laghu_configure_api_consumer(laghu_core_bounded_buffer_test core)
target_link_libraries(laghu_core_bounded_buffer_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.bounded_buffer laghu_core_bounded_buffer_test)

add_executable(laghu_core_state_transitions_test tests/core/state_transitions.cpp)
laghu_apply_first_party_contract(laghu_core_state_transitions_test)
laghu_configure_api_consumer(laghu_core_state_transitions_test core)
target_link_libraries(laghu_core_state_transitions_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.state_transitions laghu_core_state_transitions_test)

add_executable(laghu_core_deadlines_cancellation_test tests/core/deadlines_cancellation.cpp)
laghu_apply_first_party_contract(laghu_core_deadlines_cancellation_test)
laghu_configure_api_consumer(laghu_core_deadlines_cancellation_test core)
target_link_libraries(laghu_core_deadlines_cancellation_test PRIVATE laghu_core Threads::Threads)
laghu_add_native_test(laghu.core.deadlines_cancellation laghu_core_deadlines_cancellation_test)

add_executable(laghu_core_shared_offsets_test tests/core/shared_offsets.cpp)
laghu_apply_first_party_contract(laghu_core_shared_offsets_test)
laghu_configure_api_consumer(laghu_core_shared_offsets_test core)
target_link_libraries(laghu_core_shared_offsets_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.shared_offsets laghu_core_shared_offsets_test)

add_executable(laghu_core_mapped_regions_test tests/core/mapped_regions.cpp)
laghu_apply_first_party_contract(laghu_core_mapped_regions_test)
laghu_configure_api_consumer(laghu_core_mapped_regions_test core)
target_include_directories(laghu_core_mapped_regions_test PRIVATE "${CMAKE_SOURCE_DIR}/src/core/private")
target_link_libraries(laghu_core_mapped_regions_test PRIVATE laghu_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(tests/core/mapped_regions.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()
laghu_add_native_test(laghu.core.mapped_regions laghu_core_mapped_regions_test)

add_executable(laghu_core_binary_envelope_test tests/core/binary_envelope.cpp)
laghu_apply_first_party_contract(laghu_core_binary_envelope_test)
laghu_configure_api_consumer(laghu_core_binary_envelope_test core)
target_link_libraries(laghu_core_binary_envelope_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.binary_envelope laghu_core_binary_envelope_test)

laghu_add_fuzz_target(binary-envelope fuzz/binary_envelope.cpp
  SUBSYSTEM core
  LIBRARIES laghu_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # libFuzzer supplies its input only as a pointer and byte count.
  set_source_files_properties(fuzz/binary_envelope.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()
if(TARGET laghu_structured_data)
  laghu_add_fuzz_target(yyjson fuzz/yyjson.cpp
    SUBSYSTEM adapters
    LIBRARIES laghu_structured_data laghu_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # libFuzzer supplies its input only as a pointer and byte count.
    set_source_files_properties(fuzz/yyjson.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
  if(LAGHU_FUZZING_ENABLED)
    add_test(NAME laghu.fuzz.yyjson.corpus
      COMMAND "${CMAKE_SOURCE_DIR}/scripts/fuzz"
        --build "${CMAKE_BINARY_DIR}"
        --target yyjson
        --corpus "${CMAKE_SOURCE_DIR}/fuzz/corpus/yyjson"
        --runs 1)
  endif()
endif()

if(TARGET laghu_http2)
  add_executable(laghu_http2_test tests/adapters/http2.cpp)
  laghu_apply_first_party_contract(laghu_http2_test)
  laghu_configure_api_consumer(laghu_http2_test adapters)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(tests/adapters/http2.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
  target_link_libraries(laghu_http2_test PRIVATE laghu_http2 laghu_test_support)
  laghu_add_native_test(laghu.adapters.http2 laghu_http2_test)

  add_executable(laghu_http2_contract_test tests/adapters/http2_contract.cpp)
  laghu_apply_first_party_contract(laghu_http2_contract_test)
  laghu_configure_api_consumer(laghu_http2_contract_test adapters)
  laghu_add_native_test(laghu.adapters.http2.contract laghu_http2_contract_test)
endif()
if(TARGET laghu_quic)
  add_executable(laghu_http3_test tests/adapters/http3.cpp)
  laghu_apply_first_party_contract(laghu_http3_test)
  laghu_configure_api_consumer(laghu_http3_test adapters)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(tests/adapters/http3.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
  target_link_libraries(laghu_http3_test PRIVATE
    laghu_quic laghu_http3 laghu_test_support)
  laghu_add_native_test(laghu.adapters.http3 laghu_http3_test)

  add_executable(laghu_http3_contract_test tests/adapters/http3_contract.cpp)
  laghu_apply_first_party_contract(laghu_http3_contract_test)
  laghu_configure_api_consumer(laghu_http3_contract_test adapters)
  laghu_add_native_test(laghu.adapters.http3.contract laghu_http3_contract_test)
endif()
if(TARGET laghu_dns)
  add_executable(laghu_dns_test tests/adapters/dns.cpp)
  laghu_apply_first_party_contract(laghu_dns_test)
  laghu_configure_api_consumer(laghu_dns_test adapters)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(tests/adapters/dns.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
  target_link_libraries(laghu_dns_test PRIVATE
    laghu_dns laghu_test_support laghu_feature_async_dns)
  laghu_add_native_test(laghu.adapters.dns laghu_dns_test)

  add_executable(laghu_dns_contract_test tests/adapters/dns_contract.cpp)
  laghu_apply_first_party_contract(laghu_dns_contract_test)
  laghu_configure_api_consumer(laghu_dns_contract_test adapters)
  laghu_add_native_test(laghu.adapters.dns.contract laghu_dns_contract_test)
endif()
if(TARGET laghu_regex)
  add_executable(laghu_regex_test tests/adapters/regex.cpp)
  laghu_apply_first_party_contract(laghu_regex_test)
  laghu_configure_api_consumer(laghu_regex_test adapters)
  target_link_libraries(laghu_regex_test PRIVATE
    laghu_regex laghu_test_support laghu_feature_regex)
  laghu_add_native_test(laghu.adapters.regex laghu_regex_test)

  add_executable(laghu_regex_contract_test tests/adapters/regex_contract.cpp)
  laghu_apply_first_party_contract(laghu_regex_contract_test)
  laghu_configure_api_consumer(laghu_regex_contract_test adapters)
  laghu_add_native_test(laghu.adapters.regex.contract laghu_regex_contract_test)

  laghu_add_fuzz_target(pcre2 fuzz/pcre2.cpp
    SUBSYSTEM adapters
    LIBRARIES laghu_regex laghu_core laghu_feature_regex)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(fuzz/pcre2.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()
  if(LAGHU_FUZZING_ENABLED)
    add_test(NAME laghu.fuzz.pcre2.corpus
      COMMAND "${CMAKE_SOURCE_DIR}/scripts/fuzz"
        --build "${CMAKE_BINARY_DIR}"
        --target pcre2
        --corpus "${CMAKE_SOURCE_DIR}/fuzz/corpus/pcre2"
        --runs 1)
  endif()
endif()
if(TARGET laghu_codecs)
  add_executable(laghu_codecs_contract_test tests/adapters/codecs_contract.cpp)
  laghu_apply_first_party_contract(laghu_codecs_contract_test)
  laghu_configure_api_consumer(laghu_codecs_contract_test adapters)
  laghu_add_native_test(laghu.adapters.codecs.contract laghu_codecs_contract_test)

  foreach(laghu_codec_suffix IN ITEMS zlib_ng brotli zstd)
    if(TARGET "laghu_codec_${laghu_codec_suffix}")
      add_executable("laghu_codec_${laghu_codec_suffix}_test" tests/adapters/codecs.cpp)
      laghu_apply_first_party_contract("laghu_codec_${laghu_codec_suffix}_test")
      laghu_configure_api_consumer("laghu_codec_${laghu_codec_suffix}_test" adapters)
      target_compile_definitions("laghu_codec_${laghu_codec_suffix}_test" PRIVATE
        "LAGHU_CODEC_FACTORY=laghu::adapters::create_${laghu_codec_suffix}_codec"
        "LAGHU_CODEC_${laghu_codec_suffix}=1")
      target_link_libraries("laghu_codec_${laghu_codec_suffix}_test" PRIVATE
        "laghu_codec_${laghu_codec_suffix}" laghu_test_support)
      laghu_add_native_test("laghu.adapters.codec.${laghu_codec_suffix}"
        "laghu_codec_${laghu_codec_suffix}_test")
    endif()
  endforeach()
  if(TARGET laghu_codec_zlib_ng AND TARGET laghu_codec_brotli AND TARGET laghu_codec_zstd)
    add_test(NAME laghu.adapters.codecs.isolation
      COMMAND "${CMAKE_COMMAND}"
        "-DNM=${CMAKE_NM}"
        "-DZLIB_ARCHIVE=$<TARGET_FILE:laghu_codec_zlib_ng>"
        "-DBROTLI_ARCHIVE=$<TARGET_FILE:laghu_codec_brotli>"
        "-DZSTD_ARCHIVE=$<TARGET_FILE:laghu_codec_zstd>"
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectCodecIsolation.cmake")
  endif()
endif()
if(TARGET laghu_geoip)
  add_executable(laghu_geoip_contract_test tests/adapters/geoip_contract.cpp)
  laghu_apply_first_party_contract(laghu_geoip_contract_test)
  laghu_configure_api_consumer(laghu_geoip_contract_test adapters)
  laghu_add_native_test(laghu.adapters.geoip.contract laghu_geoip_contract_test)

  get_property(laghu_geoip_fixture GLOBAL PROPERTY LAGHU_LIBMAXMINDDB_FIXTURE)
  if(NOT laghu_geoip_fixture STREQUAL "" AND EXISTS "${laghu_geoip_fixture}")
    add_executable(laghu_geoip_test tests/adapters/geoip.cpp)
    laghu_apply_first_party_contract(laghu_geoip_test)
    laghu_configure_api_consumer(laghu_geoip_test adapters)
    target_compile_definitions(laghu_geoip_test PRIVATE
      "LAGHU_GEOIP_FIXTURE=\"${laghu_geoip_fixture}\"")
    target_link_libraries(laghu_geoip_test PRIVATE
      laghu_geoip laghu_test_support laghu_feature_geoip)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set_source_files_properties(tests/adapters/geoip.cpp PROPERTIES
        COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
    endif()
    laghu_add_native_test(laghu.adapters.geoip laghu_geoip_test)
  endif()
endif()
if(TARGET laghu_otlp)
  add_executable(laghu_otlp_test tests/adapters/otlp.cpp)
  laghu_apply_first_party_contract(laghu_otlp_test)
  laghu_configure_api_consumer(laghu_otlp_test adapters)
  target_include_directories(laghu_otlp_test PRIVATE "${laghu_otlp_generated}")
  target_link_libraries(laghu_otlp_test PRIVATE
    laghu_otlp laghu_otlp_schema laghu_test_support laghu_feature_otlp)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(tests/adapters/otlp.cpp PROPERTIES
      COMPILE_OPTIONS "-Wno-unsafe-buffer-usage;-Wno-keyword-macro")
  endif()
  laghu_add_native_test(laghu.adapters.otlp laghu_otlp_test)

  add_executable(laghu_otlp_contract_test tests/adapters/otlp_contract.cpp)
  laghu_apply_first_party_contract(laghu_otlp_contract_test)
  laghu_configure_api_consumer(laghu_otlp_contract_test adapters)
  laghu_add_native_test(laghu.adapters.otlp.contract laghu_otlp_contract_test)
endif()
laghu_write_fuzz_target_registry()
laghu_add_fuzz_validation_tests()

add_executable(laghu_core_digest_primitives_test tests/core/digest_primitives.cpp)
laghu_apply_first_party_contract(laghu_core_digest_primitives_test)
laghu_configure_api_consumer(laghu_core_digest_primitives_test core)
target_include_directories(laghu_core_digest_primitives_test PRIVATE "${CMAKE_SOURCE_DIR}/src/core/private")
target_link_libraries(laghu_core_digest_primitives_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.digest_primitives laghu_core_digest_primitives_test)

# This fixture compiles the adapter boundary directly without an external
# dependency. It proves the bounded translation and callback contract in the
# default minimal build; a TLS-enabled laghu_crypto test exercises OpenSSL or
# LibreSSL at the real adapter boundary below.
add_executable(laghu_adapter_dependency_error_logging_test
  tests/adapters/dependency_error_logging.cpp
  src/adapters/dependency.cpp)
laghu_apply_first_party_contract(laghu_adapter_dependency_error_logging_test)
laghu_configure_api_consumer(laghu_adapter_dependency_error_logging_test adapters)
target_include_directories(laghu_adapter_dependency_error_logging_test PRIVATE
  "${CMAKE_SOURCE_DIR}/src/adapters/private")
target_link_libraries(laghu_adapter_dependency_error_logging_test PRIVATE
  laghu_core laghu_test_support)
laghu_add_native_test(laghu.adapters.dependency_error_logging
  laghu_adapter_dependency_error_logging_test)

# The lifecycle registry is a TLS-independent adapters target. This fixture
# uses deterministic caller-owned hooks so the default minimal build proves its
# fork protocol without acquiring a TLS dependency.
add_executable(laghu_adapter_dependency_lifecycle_test tests/adapters/dependency_lifecycle.cpp)
laghu_apply_first_party_contract(laghu_adapter_dependency_lifecycle_test)
laghu_configure_api_consumer(laghu_adapter_dependency_lifecycle_test adapters)
target_sources(laghu_adapter_dependency_lifecycle_test PRIVATE
  $<TARGET_OBJECTS:laghu_dependency_lifecycle_impl>)
target_link_libraries(laghu_adapter_dependency_lifecycle_test PRIVATE laghu_core laghu_test_support)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # The fixture's bounded POSIX pipe protocol necessarily uses raw descriptors.
  set_source_files_properties(tests/adapters/dependency_lifecycle.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()
laghu_add_native_test(laghu.adapters.dependency_lifecycle
  laghu_adapter_dependency_lifecycle_test)

if(TARGET laghu_crypto)
  add_executable(laghu_crypto_provider_test tests/adapters/crypto_provider.cpp)
  laghu_apply_first_party_contract(laghu_crypto_provider_test)
  laghu_configure_api_consumer(laghu_crypto_provider_test adapters)
  target_include_directories(laghu_crypto_provider_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/adapters/private")
  target_link_libraries(laghu_crypto_provider_test PRIVATE laghu_crypto)
  target_link_libraries(laghu_crypto_provider_test PRIVATE laghu_test_support)
  laghu_add_native_test(laghu.crypto.provider laghu_crypto_provider_test)

  add_executable(laghu_crypto_entropy_seam_test tests/adapters/entropy_seam.cpp)
  laghu_apply_first_party_contract(laghu_crypto_entropy_seam_test)
  laghu_configure_api_consumer(laghu_crypto_entropy_seam_test adapters)
  target_include_directories(laghu_crypto_entropy_seam_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/adapters/private")
  target_link_libraries(laghu_crypto_entropy_seam_test PRIVATE
    laghu_crypto laghu_test_support laghu_test_time_entropy)
  laghu_add_native_test(laghu.crypto.entropy_seam laghu_crypto_entropy_seam_test)
endif()

if(TARGET laghu_idna)
  add_executable(laghu_idna_test tests/adapters/idna.cpp)
  laghu_apply_first_party_contract(laghu_idna_test)
  laghu_configure_api_consumer(laghu_idna_test adapters)
  target_include_directories(laghu_idna_test PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  target_link_libraries(laghu_idna_test PRIVATE laghu_idna laghu_feature_idna laghu_test_support)
  laghu_add_native_test(laghu.adapters.idna laghu_idna_test)
endif()

if(TARGET laghu_password_auth)
  add_executable(laghu_password_auth_test tests/adapters/password_auth.cpp)
  laghu_apply_first_party_contract(laghu_password_auth_test)
  laghu_configure_api_consumer(laghu_password_auth_test adapters)
  target_include_directories(laghu_password_auth_test PRIVATE
    "${CMAKE_SOURCE_DIR}/src/adapters/private")
  target_link_libraries(laghu_password_auth_test PRIVATE laghu_password_auth laghu_test_support)
  laghu_add_native_test(laghu.adapters.password_auth laghu_password_auth_test)

  add_executable(laghu_password_auth_failure_token_test
    tests/adapters/password_auth_failure_token.cpp)
  laghu_apply_first_party_contract(laghu_password_auth_failure_token_test)
  laghu_configure_api_consumer(laghu_password_auth_failure_token_test adapters)
  target_link_libraries(laghu_password_auth_failure_token_test PRIVATE
    laghu_password_auth laghu_test_support)
  laghu_add_native_test(laghu.adapters.password_auth.failure_token
    laghu_password_auth_failure_token_test)
endif()

if(TARGET laghu_structured_data)
  add_executable(laghu_structured_data_test tests/adapters/structured_data.cpp)
  laghu_apply_first_party_contract(laghu_structured_data_test)
  laghu_configure_api_consumer(laghu_structured_data_test adapters)
  target_link_libraries(laghu_structured_data_test PRIVATE
    laghu_structured_data laghu_test_support)
  laghu_add_native_test(laghu.adapters.structured_data laghu_structured_data_test)

  # This target intentionally does not link the yyjson adapter or dependency.
  # It proves the contract remains self-contained and does not expose yyjson.
  add_executable(laghu_structured_data_contract_test
    tests/adapters/structured_data_contract.cpp)
  laghu_apply_first_party_contract(laghu_structured_data_contract_test)
  laghu_configure_api_consumer(laghu_structured_data_contract_test adapters)
  laghu_add_native_test(laghu.adapters.structured_data.contract
    laghu_structured_data_contract_test)
endif()

laghu_add_sanitizer_fixture_targets()

add_executable(laghu_os_iovec_translation_test tests/os/iovec_translation.cpp)
laghu_apply_first_party_contract(laghu_os_iovec_translation_test)
laghu_configure_api_consumer(laghu_os_iovec_translation_test os)
target_include_directories(laghu_os_iovec_translation_test PRIVATE "${CMAKE_SOURCE_DIR}/src/os/private")
target_link_libraries(laghu_os_iovec_translation_test PRIVATE laghu_os)
target_link_libraries(laghu_os_iovec_translation_test PRIVATE laghu_test_support)
laghu_add_native_test(laghu.os.iovec_translation laghu_os_iovec_translation_test)

add_executable(laghu_runtime_event_backend_test tests/runtime/event_backend.cpp)
laghu_apply_first_party_contract(laghu_runtime_event_backend_test)
laghu_configure_api_consumer(laghu_runtime_event_backend_test runtime)
target_link_libraries(laghu_runtime_event_backend_test PRIVATE
  laghu_runtime laghu_test_support)
laghu_add_native_test(laghu.runtime.event_backend laghu_runtime_event_backend_test)

add_executable(laghu
  src/cli/main.cpp
  "${LAGHU_BUILD_MANIFEST_SOURCE}")
laghu_apply_first_party_contract(laghu)
laghu_configure_api_consumer(laghu cli)
target_include_directories(laghu PRIVATE "${CMAKE_SOURCE_DIR}/src/cli/private")
target_link_libraries(laghu PRIVATE laghu_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(src/cli/main.cpp PROPERTIES
    COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
endif()

laghu_add_capability_header_parity_target()
laghu_add_visibility_probe()
add_library(visibility_negative_fixture SHARED
  tests/api-boundaries/negative/visible-symbol.cpp)
set_property(TARGET visibility_negative_fixture PROPERTY CXX_VISIBILITY_PRESET hidden)
set_property(TARGET visibility_negative_fixture PROPERTY VISIBILITY_INLINES_HIDDEN YES)

laghu_add_validation_tests()
laghu_add_benchmark_validation_tests()
laghu_add_static_analysis_validation_tests()
laghu_add_static_analysis_target()
laghu_add_install_layout_test()

install(TARGETS laghu_core
  ARCHIVE DESTINATION lib/laghu)
install(TARGETS laghu
  RUNTIME DESTINATION bin)
install(FILES "${LAGHU_BUILD_MANIFEST}"
  DESTINATION share/laghu)

set(laghu_verify_targets
  laghu_core laghu_os laghu_runtime laghu_core_smoke laghu_core_result_errors_test
  laghu_core_strong_identifiers_test laghu_core_checked_arithmetic_test
  laghu_core_byte_text_views_test laghu_core_fd_handles_test
  laghu_core_memory_budgets_test laghu_core_bounded_arena_test
  laghu_core_slab_pools_test laghu_core_bounded_buffer_test
  laghu_core_state_transitions_test laghu_core_deadlines_cancellation_test
  laghu_core_shared_offsets_test laghu_core_mapped_regions_test
  laghu_core_binary_envelope_test laghu_core_digest_primitives_test
  laghu_os_iovec_translation_test laghu_runtime_event_backend_test
  laghu_test_support laghu_test_faults laghu_test_time_entropy
  laghu_benchmark_metrics_test laghu_benchmark_workload_counters_test laghu_benchmark_evaluate
  laghu_test_time_entropy_test laghu_test_support_fixtures_test
  laghu_test_support_runner_contract_test laghu_test_fault_injection_test laghu
  laghu_adapter_dependency_error_logging_test
  laghu_adapter_dependency_lifecycle_test
  laghu_capability_header_parity laghu_visibility_probe visibility_negative_fixture)
if(TARGET laghu_crypto_provider_test)
  list(APPEND laghu_verify_targets
    laghu_crypto laghu_crypto_provider_test laghu_crypto_entropy_seam_test)
endif()
if(TARGET laghu_idna_test)
  list(APPEND laghu_verify_targets laghu_idna laghu_idna_test)
endif()
if(TARGET laghu_password_auth_test)
  list(APPEND laghu_verify_targets
    laghu_password_auth laghu_password_auth_test laghu_password_auth_failure_token_test)
endif()
if(TARGET laghu_structured_data_test)
  list(APPEND laghu_verify_targets
    laghu_structured_data laghu_structured_data_test laghu_structured_data_contract_test)
endif()
if(TARGET laghu_http2_test)
  list(APPEND laghu_verify_targets laghu_http2 laghu_http2_test laghu_http2_contract_test)
endif()
if(TARGET laghu_http3_test)
  list(APPEND laghu_verify_targets
    laghu_quic laghu_http3 laghu_http3_test laghu_http3_contract_test)
endif()
if(TARGET laghu_dns_test)
  list(APPEND laghu_verify_targets laghu_dns laghu_dns_test laghu_dns_contract_test)
endif()
if(TARGET laghu_regex_test)
  list(APPEND laghu_verify_targets laghu_regex laghu_regex_test laghu_regex_contract_test)
endif()
if(TARGET laghu_codecs_contract_test)
  list(APPEND laghu_verify_targets laghu_codecs laghu_codecs_contract_test)
  foreach(laghu_codec_suffix IN ITEMS zlib_ng brotli zstd)
    if(TARGET "laghu_codec_${laghu_codec_suffix}_test")
      list(APPEND laghu_verify_targets
        "laghu_codec_${laghu_codec_suffix}"
        "laghu_codec_${laghu_codec_suffix}_test")
    endif()
  endforeach()
endif()
if(TARGET laghu_geoip_contract_test)
  list(APPEND laghu_verify_targets laghu_geoip laghu_geoip_contract_test)
  if(TARGET laghu_geoip_test)
    list(APPEND laghu_verify_targets laghu_geoip_test)
  endif()
endif()
if(TARGET laghu_otlp_test)
  list(APPEND laghu_verify_targets laghu_otlp laghu_otlp_schema
    laghu_otlp_test laghu_otlp_contract_test)
endif()
laghu_collect_fuzz_targets(laghu_fuzz_verify_targets)
list(APPEND laghu_verify_targets ${laghu_fuzz_verify_targets})
foreach(target IN ITEMS laghu_sanitizer_heap_misuse_fixture
    laghu_sanitizer_undefined_behavior_fixture)
  if(TARGET "${target}")
    list(APPEND laghu_verify_targets "${target}")
  endif()
endforeach()

add_custom_target(laghu_verify_toolchain
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  DEPENDS ${laghu_verify_targets}
  USES_TERMINAL
  COMMENT "Running Laghu's configured toolchain contract")

laghu_add_sanitizer_test_target()
