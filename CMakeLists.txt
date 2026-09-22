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

if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR
    "Laghu requires the Ninja generator; generator=${CMAKE_GENERATOR}. Configure with -G Ninja.")
endif()

include(CTest)
include(cmake/LaghuCapabilities.cmake)
include(cmake/LaghuDependencyDag.cmake)
include(cmake/LaghuApiBoundaries.cmake)
include(cmake/LaghuDependencies.cmake)
include(cmake/LaghuFeatures.cmake)
include(cmake/LaghuSanitizers.cmake)
include(cmake/LaghuBuildVariants.cmake)
include(cmake/LaghuBuildIdentity.cmake)
include(cmake/LaghuTestRegistration.cmake)
include(cmake/LaghuToolchain.cmake)

laghu_configure_toolchain()
laghu_configure_sanitizer_profile()
laghu_configure_build_variant()
laghu_configure_dependency_registry()
laghu_configure_dependency_modes()
laghu_configure_capability_header()
laghu_configure_build_identity()

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

laghu_declare_subsystem_graph()
laghu_configure_api_boundaries()
laghu_apply_first_party_contract(laghu_os)
laghu_configure_api_consumer(laghu_os os)
target_include_directories(laghu_os PRIVATE "${CMAKE_SOURCE_DIR}/src/os/private")

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

list(FIND LAGHU_EFFECTIVE_FEATURES tls laghu_tls_feature_index)
if(NOT laghu_tls_feature_index EQUAL -1)
  add_library(laghu_crypto STATIC
    src/adapters/crypto_provider.cpp
    src/adapters/entropy.cpp)
  laghu_apply_first_party_contract(laghu_crypto)
  laghu_configure_api_consumer(laghu_crypto adapters)
  target_include_directories(laghu_crypto PRIVATE "${CMAKE_SOURCE_DIR}/src/adapters/private")
  laghu_register_subsystem_target(laghu_crypto adapters)
  laghu_link_subsystems(laghu_crypto laghu_core)
  target_link_libraries(laghu_crypto PRIVATE laghu_feature_tls)
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

add_executable(laghu_core_digest_primitives_test tests/core/digest_primitives.cpp)
laghu_apply_first_party_contract(laghu_core_digest_primitives_test)
laghu_configure_api_consumer(laghu_core_digest_primitives_test core)
target_include_directories(laghu_core_digest_primitives_test PRIVATE "${CMAKE_SOURCE_DIR}/src/core/private")
target_link_libraries(laghu_core_digest_primitives_test PRIVATE laghu_core)
laghu_add_native_test(laghu.core.digest_primitives laghu_core_digest_primitives_test)

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

laghu_add_sanitizer_fixture_targets()

add_executable(laghu_os_iovec_translation_test tests/os/iovec_translation.cpp)
laghu_apply_first_party_contract(laghu_os_iovec_translation_test)
laghu_configure_api_consumer(laghu_os_iovec_translation_test os)
target_include_directories(laghu_os_iovec_translation_test PRIVATE "${CMAKE_SOURCE_DIR}/src/os/private")
target_link_libraries(laghu_os_iovec_translation_test PRIVATE laghu_os)
target_link_libraries(laghu_os_iovec_translation_test PRIVATE laghu_test_support)
laghu_add_native_test(laghu.os.iovec_translation laghu_os_iovec_translation_test)

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
laghu_add_install_layout_test()

install(TARGETS laghu_core
  ARCHIVE DESTINATION lib/laghu)
install(TARGETS laghu
  RUNTIME DESTINATION bin)
install(FILES "${LAGHU_BUILD_MANIFEST}"
  DESTINATION share/laghu)

set(laghu_verify_targets
  laghu_core laghu_os laghu_core_smoke laghu_core_result_errors_test
  laghu_core_strong_identifiers_test laghu_core_checked_arithmetic_test
  laghu_core_byte_text_views_test laghu_core_fd_handles_test
  laghu_core_memory_budgets_test laghu_core_bounded_arena_test
  laghu_core_slab_pools_test laghu_core_bounded_buffer_test
  laghu_core_state_transitions_test laghu_core_deadlines_cancellation_test
  laghu_core_shared_offsets_test laghu_core_mapped_regions_test
  laghu_core_binary_envelope_test laghu_core_digest_primitives_test
  laghu_os_iovec_translation_test laghu_test_support laghu_test_faults laghu_test_time_entropy
  laghu_test_time_entropy_test laghu_test_support_fixtures_test
  laghu_test_support_runner_contract_test laghu_test_fault_injection_test laghu
  laghu_capability_header_parity laghu_visibility_probe visibility_negative_fixture)
if(TARGET laghu_crypto_provider_test)
  list(APPEND laghu_verify_targets
    laghu_crypto laghu_crypto_provider_test laghu_crypto_entropy_seam_test)
endif()
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
