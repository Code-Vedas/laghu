# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

if(WIN32)
  set(LAGHU_RUNTIME_SOURCE src/runtime_win32.c)
else()
  set(LAGHU_RUNTIME_SOURCE src/runtime_posix.c)
endif()

laghu_manifest_sources(runtime LAGHU_RUNTIME_SOURCES)
add_library(laghu_runtime STATIC ${LAGHU_RUNTIME_SOURCE} ${LAGHU_RUNTIME_SOURCES})
add_library(Laghu::Runtime ALIAS laghu_runtime)

target_include_directories(laghu_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_runtime PUBLIC c_std_11)
target_link_libraries(laghu_runtime PUBLIC Laghu::Core Laghu::Image Laghu::Markup ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(laghu_runtime PUBLIC ws2_32)
endif()
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
  find_package(Threads REQUIRED)
  add_library(laghu_runtime_test_fixture STATIC tests/test_fixture.c)
  target_include_directories(laghu_runtime_test_fixture PUBLIC tests)
  foreach(subsystem IN ITEMS budget service_config queue cache operational rum source_asset
                            catalog html css javascript instrumentation lcp worker)
    add_executable(laghu_runtime_${subsystem}_test tests/${subsystem}_test.c)
    target_link_libraries(laghu_runtime_${subsystem}_test
                          PRIVATE Laghu::Runtime laghu_runtime_test_fixture)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(laghu_runtime_${subsystem}_test
                             PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME laghu_runtime_${subsystem}_test
             COMMAND laghu_runtime_${subsystem}_test)
  endforeach()
  add_executable(laghu_runtime_cache_fixture tests/cache_fixture.c)
  target_link_libraries(laghu_runtime_cache_fixture PRIVATE Laghu::Runtime)
  add_executable(laghu_runtime_integration_test tests/integration_test.c)
  add_executable(laghu_runtime_lcp_integration_test
                 tests/lcp_integration_test.c)
  add_executable(laghu_runtime_rum_integration_test
                 tests/rum_integration_test.c)
  add_executable(laghu_runtime_source_integration_test
                 tests/source_integration_test.c)
  add_executable(laghu_runtime_operational_integration_test
                 tests/operational_integration_test.c)
  target_link_libraries(laghu_runtime_operational_integration_test
                        PRIVATE Laghu::Runtime Threads::Threads)
  add_test(NAME laghu_runtime_operational_integration_test
           COMMAND laghu_runtime_operational_integration_test)
  target_link_libraries(laghu_runtime_lcp_integration_test
                        PRIVATE Laghu::Runtime Threads::Threads)
  add_test(NAME laghu_runtime_lcp_integration_test
           COMMAND laghu_runtime_lcp_integration_test)
  target_link_libraries(laghu_runtime_rum_integration_test
                        PRIVATE Laghu::Runtime laghu_runtime_test_fixture)
  add_test(NAME laghu_runtime_rum_integration_test
           COMMAND laghu_runtime_rum_integration_test)
  target_link_libraries(laghu_runtime_source_integration_test
                        PRIVATE Laghu::Runtime laghu_runtime_test_fixture)
  add_test(NAME laghu_runtime_source_integration_test
           COMMAND laghu_runtime_source_integration_test)
  target_link_libraries(laghu_runtime_integration_test
                        PRIVATE Laghu::Runtime Threads::Threads)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_runtime_integration_test
                           PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_runtime_integration_test
           COMMAND laghu_runtime_integration_test)
endif()
