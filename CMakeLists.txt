cmake_minimum_required(VERSION 3.20)

project(laghu VERSION 0.1.0 LANGUAGES C)

include(CTest)

option(LAGHU_BUILD_TESTS "Build the Laghu core test suite" ${BUILD_TESTING})

add_subdirectory(libs/laghu-core)

