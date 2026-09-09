# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.28)

project(laghu_cross_test_registration LANGUAGES NONE)

include(CTest)
set(CMAKE_CROSSCOMPILING TRUE)
include("${LAGHU_SOURCE}/cmake/LaghuTestRegistration.cmake")

add_test(NAME laghu.fixture.scripted COMMAND "${CMAKE_COMMAND}" -E true)
laghu_add_native_test(laghu.fixture.native "${CMAKE_COMMAND}" -E false)
