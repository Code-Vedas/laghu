# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.28)

project(laghu LANGUAGES CXX)

if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR
    "Laghu requires the Ninja generator; generator=${CMAKE_GENERATOR}. Configure with -G Ninja.")
endif()

include(CTest)
include(cmake/LaghuToolchain.cmake)

laghu_configure_toolchain()

add_library(laghu_core_contract OBJECT src/core/contract.cpp)
laghu_apply_first_party_contract(laghu_core_contract)
target_include_directories(laghu_core_contract PRIVATE src/core)

laghu_add_validation_tests()

add_custom_target(laghu_verify_toolchain
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  DEPENDS laghu_core_contract
  USES_TERMINAL
  COMMENT "Running Laghu's configured toolchain contract")
