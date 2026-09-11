# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_SANITIZER_PROFILE "NONE" CACHE STRING
  "Laghu sanitizer profile: NONE, ASAN_UBSAN, or TSAN")
set_property(CACHE LAGHU_SANITIZER_PROFILE PROPERTY STRINGS NONE ASAN_UBSAN TSAN)

function(laghu_sanitizer_fail detail)
  message(FATAL_ERROR "Laghu sanitizer profile failed: ${detail}")
endfunction()

function(laghu_validate_sanitizer_suppressions manifest)
  if(NOT EXISTS "${manifest}")
    laghu_sanitizer_fail("manifest=${manifest}; file is required")
  endif()
  file(STRINGS "${manifest}" lines)
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#" OR line STREQUAL "")
      continue()
    endif()
    string(REPLACE "\t" ";" fields "${line}")
    list(LENGTH fields field_count)
    if(field_count EQUAL 3)
      list(APPEND fields "")
    elseif(NOT field_count EQUAL 4)
      laghu_sanitizer_fail("manifest=${manifest}; expected=sanitizer-platform-target_or_source-reason")
    endif()
    list(GET fields 0 sanitizer)
    list(GET fields 1 platform)
    list(GET fields 2 target_or_source)
    list(GET fields 3 reason)
    if(NOT sanitizer STREQUAL "address" AND NOT sanitizer STREQUAL "undefined" AND
        NOT sanitizer STREQUAL "thread")
      laghu_sanitizer_fail("sanitizer=${sanitizer}; expected=address-undefined-or-thread")
    endif()
    if(NOT platform STREQUAL "linux")
      laghu_sanitizer_fail("platform=${platform}; expected=linux")
    endif()
    if(target_or_source MATCHES "(^|/)(all|global|third_party|vendor|external)(/|$)" OR
        target_or_source MATCHES "[*?]")
      laghu_sanitizer_fail("target_or_source=${target_or_source}; global_or_third_party_suppression_is_forbidden")
    endif()
    if(NOT target_or_source MATCHES "^(laghu_[A-Za-z0-9_]+|src/(core|config|protocol|tls|proxy|cache|control|cli|observability|os)/[A-Za-z0-9_./-]+\\.(cpp|cc|cxx))$")
      laghu_sanitizer_fail("target_or_source=${target_or_source}; expected=one_laghu_target_or_governed_source")
    endif()
    string(STRIP "${reason}" reason)
    if(reason STREQUAL "")
      laghu_sanitizer_fail("target_or_source=${target_or_source}; technical_reason_required")
    endif()
  endforeach()
endfunction()

function(laghu_configure_sanitizer_profile)
  if(NOT LAGHU_SANITIZER_PROFILE STREQUAL "NONE" AND
      NOT LAGHU_SANITIZER_PROFILE STREQUAL "ASAN_UBSAN" AND
      NOT LAGHU_SANITIZER_PROFILE STREQUAL "TSAN")
    laghu_sanitizer_fail("profile=${LAGHU_SANITIZER_PROFILE}; expected=NONE-ASAN_UBSAN-or-TSAN")
  endif()
  laghu_validate_sanitizer_suppressions("${CMAKE_SOURCE_DIR}/tests/sanitizers/suppressions.tsv")

  set(compile_options)
  set(link_options)
  if(NOT LAGHU_SANITIZER_PROFILE STREQUAL "NONE")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      laghu_sanitizer_fail("profile=${LAGHU_SANITIZER_PROFILE}; requires=linux_clang")
    endif()
    if(LAGHU_SANITIZER_PROFILE STREQUAL "ASAN_UBSAN")
      set(compile_options -fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined)
      set(link_options -fsanitize=address,undefined)
    else()
      set(compile_options -fsanitize=thread -fno-omit-frame-pointer)
      set(link_options -fsanitize=thread)
    endif()
  endif()
  set(LAGHU_SANITIZER_COMPILE_OPTIONS "${compile_options}" CACHE INTERNAL
    "Laghu sanitizer compile options" FORCE)
  set(LAGHU_SANITIZER_LINK_OPTIONS "${link_options}" CACHE INTERNAL
    "Laghu sanitizer link options" FORCE)
endfunction()

function(laghu_add_sanitizer_fixture_targets)
  if(LAGHU_SANITIZER_PROFILE STREQUAL "NONE" OR CMAKE_CROSSCOMPILING)
    return()
  endif()
  if(LAGHU_SANITIZER_PROFILE STREQUAL "ASAN_UBSAN")
    foreach(fixture IN ITEMS heap_misuse undefined_behavior)
      set(target "laghu_sanitizer_${fixture}_fixture")
      add_executable("${target}" "${CMAKE_SOURCE_DIR}/tests/sanitizers/${fixture}.cpp")
      laghu_apply_first_party_contract("${target}")
      laghu_configure_api_consumer("${target}" core)
      if(fixture STREQUAL "heap_misuse" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # This isolated fixture must perform an unchecked access for ASan to detect.
        set_source_files_properties("${CMAKE_SOURCE_DIR}/tests/sanitizers/heap_misuse.cpp"
          PROPERTIES COMPILE_OPTIONS -Wno-unsafe-buffer-usage)
      endif()
      add_test(NAME "laghu.sanitizer.fixture.${fixture}"
        COMMAND "${CMAKE_COMMAND}"
          "-DEXECUTABLE=$<TARGET_FILE:${target}>"
          "-DENVIRONMENT_1=ASAN_OPTIONS=halt_on_error=1"
          "-DENVIRONMENT_2=UBSAN_OPTIONS=halt_on_error=1"
          "-DEXPECTED=${fixture}"
          -P "${CMAKE_SOURCE_DIR}/cmake/ExpectSanitizerFailure.cmake")
    endforeach()
  else()
    add_executable(laghu_core_deadlines_cancellation_tsan_test
      tests/core/deadlines_cancellation.cpp)
    laghu_apply_first_party_contract(laghu_core_deadlines_cancellation_tsan_test)
    laghu_configure_api_consumer(laghu_core_deadlines_cancellation_tsan_test core)
    target_link_libraries(laghu_core_deadlines_cancellation_tsan_test PRIVATE laghu_core Threads::Threads)
    laghu_add_native_test(laghu.core.deadlines_cancellation.tsan
      laghu_core_deadlines_cancellation_tsan_test)

    add_executable(laghu_sanitizer_data_race_fixture
      "${CMAKE_SOURCE_DIR}/tests/sanitizers/data_race.cpp")
    laghu_apply_first_party_contract(laghu_sanitizer_data_race_fixture)
    target_link_libraries(laghu_sanitizer_data_race_fixture PRIVATE Threads::Threads)
    add_test(NAME laghu.sanitizer.fixture.data_race
      COMMAND "${CMAKE_COMMAND}"
        "-DEXECUTABLE=$<TARGET_FILE:laghu_sanitizer_data_race_fixture>"
        "-DENVIRONMENT_1=TSAN_OPTIONS=halt_on_error=1"
        -DEXPECTED=data_race
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectSanitizerFailure.cmake")
  endif()
endfunction()

function(laghu_add_sanitizer_test_target)
  if(LAGHU_SANITIZER_PROFILE STREQUAL "TSAN" AND NOT CMAKE_CROSSCOMPILING)
    add_custom_target(laghu_sanitizer_tsan_tests
      COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
        -R "laghu.core.deadlines_cancellation.tsan"
      COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
        -R "laghu.sanitizer.fixture.data_race"
      DEPENDS laghu_core_deadlines_cancellation_tsan_test laghu_sanitizer_data_race_fixture
      USES_TERMINAL
      COMMENT "Running Laghu concurrency tests under ThreadSanitizer")
  endif()
endfunction()
