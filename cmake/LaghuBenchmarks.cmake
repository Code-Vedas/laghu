# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_BENCHMARK_SCHEMA_VERSION laghu-benchmark-v1)

function(laghu_benchmark_fail detail)
  message(FATAL_ERROR "Laghu benchmark configuration failed: ${detail}")
endfunction()

function(laghu_configure_benchmarks)
  if(NOT DEFINED LAGHU_BUILD_ID OR LAGHU_BUILD_ID STREQUAL "")
    laghu_benchmark_fail("rule=build_identity_missing")
  endif()
  if(NOT DEFINED LAGHU_BUILD_IDENTITY_FEATURES_JSON OR
      NOT DEFINED LAGHU_BUILD_IDENTITY_DEPENDENCIES_JSON)
    laghu_benchmark_fail("rule=build_identity_components_missing")
  endif()

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/laghu")
  set(identity_header "${CMAKE_BINARY_DIR}/generated/laghu/benchmark_identity.hpp")
  file(WRITE "${identity_header}"
"// SPDX-License-Identifier: AGPL-3.0-only\n#pragma once\n\n#include <string_view>\n\nnamespace laghu::benchmark::internal {\ninline constexpr std::string_view schema_version = \"${LAGHU_BENCHMARK_SCHEMA_VERSION}\";\ninline constexpr std::string_view build_id = \"${LAGHU_BUILD_ID}\";\ninline constexpr std::string_view compiler_id = \"${CMAKE_CXX_COMPILER_ID}\";\ninline constexpr std::string_view compiler_version = \"${CMAKE_CXX_COMPILER_VERSION}\";\ninline constexpr std::string_view target_architecture = \"${CMAKE_SYSTEM_PROCESSOR}\";\ninline constexpr std::string_view target_os = \"${CMAKE_SYSTEM_NAME}\";\ninline constexpr std::string_view features_json = R\"laghu(${LAGHU_BUILD_IDENTITY_FEATURES_JSON})laghu\";\ninline constexpr std::string_view dependencies_json = R\"laghu(${LAGHU_BUILD_IDENTITY_DEPENDENCIES_JSON})laghu\";\n}  // namespace laghu::benchmark::internal\n")
  set(LAGHU_BENCHMARK_IDENTITY_HEADER "${identity_header}" CACHE INTERNAL
    "Laghu build-local benchmark identity header" FORCE)
endfunction()

function(laghu_add_benchmark_targets)
  if(NOT DEFINED LAGHU_BENCHMARK_IDENTITY_HEADER)
    laghu_benchmark_fail("rule=identity_header_missing")
  endif()

  add_executable(laghu_benchmark_core_foundation EXCLUDE_FROM_ALL
    bench/core_foundation.cpp
    bench/runner.cpp)
  laghu_apply_first_party_contract(laghu_benchmark_core_foundation)
  laghu_configure_api_consumer(laghu_benchmark_core_foundation core)
  target_include_directories(laghu_benchmark_core_foundation PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/bench/private")
  target_link_libraries(laghu_benchmark_core_foundation PRIVATE laghu_core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # The runner's bounded POSIX argv, write, sysctl, and /proc buffers require
    # pointer-based APIs. Keep the waiver confined to this non-installed tool.
    set_source_files_properties(bench/runner.cpp PROPERTIES
      COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
  endif()

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/config/laghu-benchmarks-v1.tsv"
    CONTENT "# laghu-benchmarks-v1\n# workload\tcmake_target\texecutable\ncore-foundation\tlaghu_benchmark_core_foundation\t$<TARGET_FILE:laghu_benchmark_core_foundation>\n")

  add_executable(laghu_benchmark_metrics_test tests/benchmarks/metrics.cpp)
  laghu_apply_first_party_contract(laghu_benchmark_metrics_test)
  target_include_directories(laghu_benchmark_metrics_test PRIVATE
    "${CMAKE_SOURCE_DIR}/bench/private")
  target_link_libraries(laghu_benchmark_metrics_test PRIVATE laghu_test_support)
  laghu_add_native_test(laghu.benchmark.metrics laghu_benchmark_metrics_test)
endfunction()

function(laghu_add_benchmark_validation_tests)
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()
  add_test(NAME laghu.benchmark.runner
    COMMAND "${CMAKE_COMMAND}"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/benchmark"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
      "-DSOURCE_DIRECTORY=${CMAKE_SOURCE_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectBenchmarkRunner.cmake")
  add_test(NAME laghu.benchmark.release_exclusion
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
      "-DARCHIVE=$<TARGET_FILE:laghu_core>"
      "-DEXECUTABLE=$<TARGET_FILE:laghu>"
      "-DNM=${CMAKE_NM}"
      "-DSTAGE_DIRECTORY=${CMAKE_BINARY_DIR}/tests/benchmark-install"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectBenchmarkReleaseExclusion.cmake")
endfunction()
