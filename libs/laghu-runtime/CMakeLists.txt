# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

laghu_manifest_sources(runtime LAGHU_RUNTIME_SOURCES)
add_library(laghu_runtime STATIC src/runtime_posix.c ${LAGHU_RUNTIME_SOURCES})
add_library(Laghu::Runtime ALIAS laghu_runtime)
set_property(SOURCE src/runtime_posix.c ${LAGHU_RUNTIME_SOURCES} APPEND PROPERTY
             OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/persisted_state_contract.def")
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BROTLIENC REQUIRED IMPORTED_TARGET libbrotlienc)

target_include_directories(laghu_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_runtime PUBLIC c_std_11)
target_link_libraries(laghu_runtime PUBLIC Laghu::Core Laghu::Image Laghu::Markup
                      ZLIB::ZLIB PkgConfig::BROTLIENC ${CMAKE_DL_LIBS})
set_target_properties(laghu_runtime PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LAGHU_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(laghu_runtime_fuzz tests/runtime_fuzz.c)
  target_link_libraries(laghu_runtime_fuzz PRIVATE Laghu::Runtime)
  target_compile_options(laghu_runtime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(laghu_runtime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(LAGHU_BUILD_TESTS)
  function(laghu_add_runtime_test logical_name)
    cmake_parse_arguments(ARG "THREADS;FIXTURE" "" "COMMAND_ARGS" ${ARGN})
    laghu_manifest_source(runtime_test ${logical_name} source)
    add_executable(laghu_runtime_${logical_name}_test ${source})
    if(ARG_FIXTURE)
      target_link_libraries(laghu_runtime_${logical_name}_test
                            PRIVATE laghu_runtime_test_fixture)
    else()
      target_link_libraries(laghu_runtime_${logical_name}_test
                            PRIVATE Laghu::Runtime)
    endif()
    if(ARG_THREADS)
      find_package(Threads REQUIRED)
      target_link_libraries(laghu_runtime_${logical_name}_test
                            PRIVATE Threads::Threads)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(laghu_runtime_${logical_name}_test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME laghu_runtime_${logical_name}_test
             COMMAND laghu_runtime_${logical_name}_test ${ARG_COMMAND_ARGS})
  endfunction()

  laghu_manifest_source(runtime_test fixture_support fixture_source)
  add_library(laghu_runtime_test_fixture STATIC ${fixture_source})
  target_include_directories(laghu_runtime_test_fixture PUBLIC tests)
  target_link_libraries(laghu_runtime_test_fixture PUBLIC Laghu::Runtime)
  foreach(subsystem IN ITEMS budget service_config queue cache precompressed operational rum source_asset
                            catalog html html_cache css domain javascript layout instrumentation profile lcp worker log trace)
    laghu_add_runtime_test(${subsystem} FIXTURE)
  endforeach()
  laghu_add_runtime_test(fixture FIXTURE)
  laghu_manifest_source(runtime_test cache_fixture cache_fixture_source)
  add_executable(laghu_runtime_cache_fixture ${cache_fixture_source})
  target_link_libraries(laghu_runtime_cache_fixture PRIVATE Laghu::Runtime)
  laghu_add_runtime_test(operational_integration THREADS FIXTURE)
  laghu_add_runtime_test(rum_integration FIXTURE)
  laghu_add_runtime_test(source_integration FIXTURE)
  laghu_add_runtime_test(html_integration FIXTURE)
  laghu_add_runtime_test(asset_integration FIXTURE)
  laghu_add_runtime_test(cache_catalog_integration FIXTURE)
  laghu_add_runtime_test(css_critical_integration FIXTURE)
  laghu_add_runtime_test(javascript_integration FIXTURE)
  laghu_add_runtime_test(instrumentation_lcp_integration FIXTURE)
  laghu_add_runtime_test(chrome_analysis_concurrency)
endif()
