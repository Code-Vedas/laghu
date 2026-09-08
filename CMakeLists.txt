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
include(cmake/LaghuToolchain.cmake)

laghu_configure_toolchain()
laghu_configure_capability_header()

add_library(laghu_core STATIC src/core/contract.cpp)
laghu_apply_first_party_contract(laghu_core)
target_include_directories(laghu_core PRIVATE
  src/core
  "${CMAKE_BINARY_DIR}/generated")

add_executable(laghu_core_smoke src/core/smoke.cpp)
laghu_apply_first_party_contract(laghu_core_smoke)
target_link_libraries(laghu_core_smoke PRIVATE laghu_core)

laghu_add_capability_header_parity_target()

laghu_add_validation_tests()
laghu_add_install_layout_test()

install(TARGETS laghu_core
  ARCHIVE DESTINATION lib/laghu)

add_custom_target(laghu_verify_toolchain
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  DEPENDS laghu_core laghu_core_smoke laghu_capability_header_parity
  USES_TERMINAL
  COMMENT "Running Laghu's configured toolchain contract")
