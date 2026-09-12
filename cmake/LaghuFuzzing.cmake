# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_BUILD_FUZZERS OFF CACHE BOOL "Build Laghu Clang libFuzzer targets")

function(laghu_fuzz_fail detail)
  message(FATAL_ERROR "Laghu fuzz configuration failed: ${detail}")
endfunction()

function(laghu_configure_fuzzing)
  set(LAGHU_FUZZING_ENABLED OFF CACHE INTERNAL "Laghu fuzzing is enabled" FORCE)
  if(NOT LAGHU_BUILD_FUZZERS)
    return()
  endif()
  if(CMAKE_CROSSCOMPILING)
    laghu_fuzz_fail("build_fuzzers=ON; requires=native_configuration")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    laghu_fuzz_fail("build_fuzzers=ON; requires=clang_libfuzzer")
  endif()
  if(LAGHU_SANITIZER_PROFILE STREQUAL "TSAN")
    laghu_fuzz_fail("build_fuzzers=ON; incompatible_sanitizer_profile=TSAN")
  endif()
  if(NOT LAGHU_SANITIZER_PROFILE STREQUAL "ASAN_UBSAN")
    laghu_fuzz_fail("build_fuzzers=ON; requires=sanitizer_profile_ASAN_UBSAN")
  endif()
  set(laghu_libfuzzer_probe_directory "${CMAKE_BINARY_DIR}/probes/try-libfuzzer")
  try_compile(laghu_libfuzzer_available
    "${laghu_libfuzzer_probe_directory}"
    SOURCES "${CMAKE_SOURCE_DIR}/tests/configure/probes/libfuzzer.cpp"
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=23"
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
      "-DCMAKE_CXX_EXTENSIONS=OFF"
    COMPILE_DEFINITIONS -fsanitize=fuzzer,address,undefined
    LINK_OPTIONS -fsanitize=fuzzer,address,undefined
    OUTPUT_VARIABLE laghu_libfuzzer_output)
  laghu_sanitize_probe_text(laghu_sanitized_libfuzzer_output "${laghu_libfuzzer_output}")
  file(WRITE "${CMAKE_BINARY_DIR}/probes/libfuzzer.log" "${laghu_sanitized_libfuzzer_output}")
  if(NOT laghu_libfuzzer_available)
    laghu_fuzz_fail("build_fuzzers=ON; requires=clang_libfuzzer_runtime; retained_log=probes/libfuzzer.log")
  endif()
  set(LAGHU_FUZZING_ENABLED ON CACHE INTERNAL "Laghu fuzzing is enabled" FORCE)
endfunction()

function(laghu_add_fuzz_target name source)
  if(NOT LAGHU_FUZZING_ENABLED)
    return()
  endif()
  if(NOT name MATCHES "^[a-z][a-z0-9-]*$")
    laghu_fuzz_fail("target=${name}; expected=lowercase_hyphenated_id")
  endif()
  cmake_parse_arguments(FUZZ "" "SUBSYSTEM" "LIBRARIES" ${ARGN})
  if(FUZZ_SUBSYSTEM STREQUAL "" OR FUZZ_LIBRARIES STREQUAL "")
    laghu_fuzz_fail("target=${name}; subsystem_and_libraries_are_required")
  endif()
  string(REPLACE "-" "_" target_suffix "${name}")
  set(target "laghu_fuzz_${target_suffix}")
  add_executable("${target}" EXCLUDE_FROM_ALL "${source}")
  laghu_apply_first_party_contract("${target}")
  laghu_configure_api_consumer("${target}" "${FUZZ_SUBSYSTEM}")
  target_link_libraries("${target}" PRIVATE ${FUZZ_LIBRARIES})
  target_compile_options("${target}" PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)
  target_link_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
  set_property(TARGET "${target}" PROPERTY LAGHU_FUZZ_NAME "${name}")
  set_property(GLOBAL APPEND PROPERTY LAGHU_FUZZ_TARGETS "${target}")
endfunction()

function(laghu_write_fuzz_target_registry)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  get_property(fuzz_targets GLOBAL PROPERTY LAGHU_FUZZ_TARGETS)
  set(registry "# laghu-fuzz-targets-v1\n# target\tcmake_target\texecutable\n")
  foreach(target IN LISTS fuzz_targets)
    get_property(name TARGET "${target}" PROPERTY LAGHU_FUZZ_NAME)
    string(APPEND registry "${name}\t${target}\t$<TARGET_FILE:${target}>\n")
  endforeach()
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/config/laghu-fuzz-targets-v1.tsv"
    CONTENT "${registry}")
endfunction()

function(laghu_add_fuzz_validation_tests)
  if(NOT LAGHU_FUZZING_ENABLED)
    return()
  endif()
  add_test(NAME laghu.fuzz.wrapper
    COMMAND "${CMAKE_COMMAND}"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/fuzz"
      "-DTEST_BUILD=${CMAKE_BINARY_DIR}"
      "-DCORPUS=${CMAKE_SOURCE_DIR}/fuzz/corpus/binary-envelope"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectFuzzWrapper.cmake")
  add_test(NAME laghu.fuzz.release_exclusion
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
      "-DARCHIVE=$<TARGET_FILE:laghu_core>"
      "-DEXECUTABLE=$<TARGET_FILE:laghu>"
      "-DFUZZER=$<TARGET_FILE:laghu_fuzz_binary_envelope>"
      "-DFUZZER_TARGET=laghu_fuzz_binary_envelope"
      "-DNM=${CMAKE_NM}"
      "-DSTAGE_DIRECTORY=${CMAKE_BINARY_DIR}/tests/fuzz-install"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectFuzzReleaseExclusion.cmake")
  add_test(NAME laghu.fuzz.default_all_exclusion
    COMMAND "${CMAKE_COMMAND}"
      "-DSOURCE=${CMAKE_SOURCE_DIR}"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}/tests/fuzz-default-all"
      "-DCXX=${CMAKE_CXX_COMPILER}"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/scripts/fuzz"
      "-DCORPUS=${CMAKE_SOURCE_DIR}/fuzz/corpus/binary-envelope"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectFuzzDefaultAllExclusion.cmake")
endfunction()

function(laghu_collect_fuzz_targets output)
  get_property(fuzz_targets GLOBAL PROPERTY LAGHU_FUZZ_TARGETS)
  set(${output} "${fuzz_targets}" PARENT_SCOPE)
endfunction()
