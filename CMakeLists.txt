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
include(cmake/LaghuBuildVariants.cmake)
include(cmake/LaghuToolchain.cmake)

laghu_configure_toolchain()
laghu_configure_build_variant()
laghu_configure_dependency_registry()
laghu_configure_dependency_modes()
laghu_configure_capability_header()

add_library(laghu_core STATIC src/core/contract.cpp)
laghu_apply_first_party_contract(laghu_core)
target_link_libraries(laghu_core PRIVATE laghu_feature_core)

laghu_declare_subsystem_graph()
laghu_configure_api_boundaries()

add_executable(laghu_core_smoke src/core/smoke.cpp)
laghu_apply_first_party_contract(laghu_core_smoke)
laghu_configure_api_consumer(laghu_core_smoke core)
target_link_libraries(laghu_core_smoke PRIVATE laghu_core)

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

add_custom_target(laghu_verify_toolchain
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  DEPENDS laghu_core laghu_core_smoke laghu_capability_header_parity laghu_visibility_probe visibility_negative_fixture
  USES_TERMINAL
  COMMENT "Running Laghu's configured toolchain contract")
