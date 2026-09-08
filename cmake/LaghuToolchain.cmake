# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

set(LAGHU_PROBE_DIRECTORY "${CMAKE_BINARY_DIR}/probes")
file(MAKE_DIRECTORY "${LAGHU_PROBE_DIRECTORY}")

function(laghu_sanitize_probe_text output input)
  set(value "${input}")
  string(REPLACE "${CMAKE_SOURCE_DIR}" "<source>" value "${value}")
  string(REPLACE "${CMAKE_BINARY_DIR}" "<build>" value "${value}")
  string(REPLACE "${CMAKE_CURRENT_BINARY_DIR}" "<build>" value "${value}")
  string(REGEX REPLACE "cmTC_[A-Za-z0-9]+" "cmTC_<id>" value "${value}")
  set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(laghu_fail capability detail)
  message(FATAL_ERROR
    "Laghu configuration failed: capability=${capability} target_os=${CMAKE_SYSTEM_NAME} "
    "compiler=${CMAKE_CXX_COMPILER_ID} compiler_version=${CMAKE_CXX_COMPILER_VERSION} "
    "standard_library=${LAGHU_STANDARD_LIBRARY_ID} "
    "standard_library_version=${LAGHU_STANDARD_LIBRARY_VERSION} ${detail}")
endfunction()

function(laghu_detect_standard_library)
  set(identity_source "${LAGHU_PROBE_DIRECTORY}/standard-library-identity.cpp")
  file(WRITE "${identity_source}" "#include <version>\n")
  separate_arguments(identity_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS}")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -std=c++23 ${identity_flags} -dM -E -x c++ "${identity_source}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE definitions
    ERROR_VARIABLE diagnostics)
  laghu_sanitize_probe_text(sanitized_diagnostics "${diagnostics}")
  file(WRITE "${LAGHU_PROBE_DIRECTORY}/standard-library-identity.log" "${sanitized_diagnostics}")
  if(NOT result EQUAL 0)
    set(LAGHU_STANDARD_LIBRARY_ID unknown PARENT_SCOPE)
    set(LAGHU_STANDARD_LIBRARY_VERSION unknown PARENT_SCOPE)
    laghu_fail(standard_library_identity "retained_log=probes/standard-library-identity.log")
  endif()
  if(definitions MATCHES "#define _LIBCPP_VERSION ([0-9]+)")
    set(LAGHU_STANDARD_LIBRARY_ID libc++ PARENT_SCOPE)
    set(LAGHU_STANDARD_LIBRARY_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
  elseif(definitions MATCHES "#define _GLIBCXX_RELEASE ([0-9]+)")
    set(LAGHU_STANDARD_LIBRARY_ID libstdc++ PARENT_SCOPE)
    set(LAGHU_STANDARD_LIBRARY_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
  elseif(definitions MATCHES "#define __GLIBCXX__ ([0-9]+)")
    set(LAGHU_STANDARD_LIBRARY_ID libstdc++ PARENT_SCOPE)
    set(LAGHU_STANDARD_LIBRARY_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
  else()
    set(LAGHU_STANDARD_LIBRARY_ID unknown PARENT_SCOPE)
    set(LAGHU_STANDARD_LIBRARY_VERSION unknown PARENT_SCOPE)
    laghu_fail(standard_library_identity "retained_log=probes/standard-library-identity.log")
  endif()
endfunction()

function(laghu_compile_probe capability source require_link result_variable)
  set(probe_binary_dir "${LAGHU_PROBE_DIRECTORY}/try-${capability}")
  set(probe_flags
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    "-DCMAKE_CXX_EXTENSIONS=OFF"
    "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF")
  if(NOT require_link OR CMAKE_CROSSCOMPILING)
    set(saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  endif()
  set(probe_definitions)
  if(capability STREQUAL "posix_2008" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND probe_definitions -DLAGHU_POSIX_REQUIRE_NUMERIC)
  endif()
  try_compile(result "${probe_binary_dir}" SOURCES "${source}"
    COMPILE_DEFINITIONS ${probe_definitions}
    CMAKE_FLAGS ${probe_flags} OUTPUT_VARIABLE output)
  if(NOT require_link)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${saved_try_compile_target_type}")
  endif()
  laghu_sanitize_probe_text(sanitized_output "${output}")
  file(WRITE "${LAGHU_PROBE_DIRECTORY}/${capability}.log" "${sanitized_output}")
  if(NOT result)
    laghu_fail("${capability}" "retained_log=probes/${capability}.log")
  endif()
  set(${result_variable} true PARENT_SCOPE)
endfunction()

function(laghu_warning_probe warning)
  string(MAKE_C_IDENTIFIER "${warning}" warning_id)
  set(probe_binary_dir "${LAGHU_PROBE_DIRECTORY}/warning-${warning_id}")
  set(saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  try_compile(result "${probe_binary_dir}"
    SOURCES "${CMAKE_SOURCE_DIR}/tests/warnings/probes/clean.cpp"
    COMPILE_DEFINITIONS -Wall -Werror "${warning}"
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=23"
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
      "-DCMAKE_CXX_EXTENSIONS=OFF"
      "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
    OUTPUT_VARIABLE output)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE "${saved_try_compile_target_type}")
  laghu_sanitize_probe_text(sanitized_output "${output}")
  file(WRITE "${LAGHU_PROBE_DIRECTORY}/warning-${warning_id}.log" "${sanitized_output}")
  if(result)
    if(LAGHU_EFFECTIVE_WARNING_FLAGS STREQUAL "")
      set(updated_warning_flags "${warning}")
    else()
      set(updated_warning_flags "${LAGHU_EFFECTIVE_WARNING_FLAGS};${warning}")
    endif()
    set(LAGHU_EFFECTIVE_WARNING_FLAGS "${updated_warning_flags}" PARENT_SCOPE)
  endif()
endfunction()

function(laghu_require_no_raw_extensions)
  file(GLOB_RECURSE owned_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.cc"
    "${CMAKE_SOURCE_DIR}/src/*.cxx" "${CMAKE_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_SOURCE_DIR}/src/*.hh" "${CMAKE_SOURCE_DIR}/src/*.hxx")
  foreach(source IN LISTS owned_sources)
    if(source STREQUAL "${CMAKE_SOURCE_DIR}/src/core/compiler_extensions.hpp")
      continue()
    endif()
    file(READ "${source}" contents)
    if(contents MATCHES "__builtin_[A-Za-z0-9_]+|__attribute__[ \t\r\n]*\\(|__declspec[ \t\r\n]*\\(")
      file(RELATIVE_PATH relative_source "${CMAKE_SOURCE_DIR}" "${source}")
      laghu_fail(raw_compiler_extension "file=${relative_source}; only src/core/compiler_extensions.hpp is allowed")
    endif()
  endforeach()
endfunction()

function(laghu_require_core_profile_sources)
  set(governed_roots core config protocol tls proxy cache control cli observability os)
  set(governed_sources)
  foreach(root IN LISTS governed_roots)
    file(GLOB_RECURSE root_sources CONFIGURE_DEPENDS
      "${CMAKE_SOURCE_DIR}/src/${root}/*.cpp" "${CMAKE_SOURCE_DIR}/src/${root}/*.cc"
      "${CMAKE_SOURCE_DIR}/src/${root}/*.cxx" "${CMAKE_SOURCE_DIR}/src/${root}/*.hpp"
      "${CMAKE_SOURCE_DIR}/src/${root}/*.hh" "${CMAKE_SOURCE_DIR}/src/${root}/*.hxx")
    list(APPEND governed_sources ${root_sources})
  endforeach()
  foreach(source IN LISTS governed_sources)
    file(READ "${source}" contents)
    file(RELATIVE_PATH relative_source "${CMAKE_SOURCE_DIR}" "${source}")
    foreach(rule_pattern IN ITEMS
        "exceptions|(throw|try|catch)"
        "rtti|(dynamic_cast|typeid|std::type_info|type_info)"
        "coroutines|(co_await|co_return|co_yield)"
        "futures|(std::future|std::shared_future|std::promise|std::async|std::packaged_task|future|shared_future|promise)")
      string(REPLACE "|" ";" rule_pattern_parts "${rule_pattern}")
      list(GET rule_pattern_parts 0 rule)
      list(REMOVE_AT rule_pattern_parts 0)
      list(JOIN rule_pattern_parts "|" pattern)
      if(contents MATCHES "${pattern}")
        laghu_fail("restricted_profile_${rule}" "file=${relative_source}")
      endif()
    endforeach()
    foreach(header_rule IN ITEMS "typeinfo|rtti" "coroutine|coroutines" "future|futures" "iostream|iostream")
      string(REPLACE "|" ";" header_rule_parts "${header_rule}")
      list(GET header_rule_parts 0 header)
      list(GET header_rule_parts 1 rule)
      if(contents MATCHES "#[ \t]*include[ \t]*[<\\\"]${header}[>\\\"]")
        laghu_fail("restricted_profile_${rule}" "file=${relative_source}")
      endif()
    endforeach()
    if(contents MATCHES "(^|[^A-Za-z0-9_])virtual[ \t\r\n]")
      laghu_fail(restricted_profile_virtual "file=${relative_source}")
    endif()
  endforeach()
endfunction()

function(laghu_validate_warning_suppressions manifest)
  file(STRINGS "${manifest}" lines)
  foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#" OR line STREQUAL "")
      continue()
    endif()
    string(REPLACE "\t" ";" fields "${line}")
    list(LENGTH fields field_count)
    if(NOT field_count EQUAL 4)
      laghu_fail(warning_suppression "manifest=${manifest}; expected=source-warning-compiler-reason")
    endif()
    list(GET fields 0 source)
    list(GET fields 1 warning)
    list(GET fields 2 compiler_condition)
    list(GET fields 3 reason)
    if(NOT source MATCHES "^src/(core|config|protocol|tls|proxy|cache|control|cli|observability|os)/[A-Za-z0-9_./-]+\\.(cpp|cc|cxx)$" OR source MATCHES "[*?]" OR NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
      laghu_fail(warning_suppression "source=${source}; source must name one governed implementation file")
    endif()
    if(NOT warning MATCHES "^-Wno-(conversion|sign-conversion|shadow|format|format-security|null-dereference|double-promotion|implicit-fallthrough|cast-align|cast-qual|old-style-cast|overloaded-virtual|non-virtual-dtor|zero-as-null-pointer-constant|undef|uninitialized|arith-conversion|dangling-pointer|format-overflow|format-truncation|array-bounds|stringop-overflow|duplicated-cond|logical-op|useless-cast|shadow-all|implicit-int-conversion|shorten-64-to-32|dangling|unsafe-buffer-usage)$")
      laghu_fail(warning_suppression "warning=${warning}; warning is not an approved source-level suppression")
    endif()
    if(NOT compiler_condition STREQUAL "all" AND NOT compiler_condition STREQUAL "${LAGHU_COMPILER_FAMILY}")
      laghu_fail(warning_suppression "compiler_condition=${compiler_condition}; expected=all-or-current-compiler")
    endif()
    if(reason STREQUAL "")
      laghu_fail(warning_suppression "source=${source}; technical reason is required")
    endif()
  endforeach()
endfunction()

function(laghu_write_metadata)
  list(JOIN LAGHU_EFFECTIVE_WARNING_FLAGS "\", \"" warnings_json)
  laghu_capability_json_members(capabilities_json)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/config")
  file(WRITE "${CMAKE_BINARY_DIR}/config/laghu-config-v1.json"
"{\n  \"schema_version\": \"laghu-config-v1\",\n  \"target_os\": \"${CMAKE_SYSTEM_NAME}\",\n  \"compiler\": {\"id\": \"${CMAKE_CXX_COMPILER_ID}\", \"version\": \"${CMAKE_CXX_COMPILER_VERSION}\"},\n  \"standard_library\": {\"id\": \"${LAGHU_STANDARD_LIBRARY_ID}\", \"version\": \"${LAGHU_STANDARD_LIBRARY_VERSION}\"},\n  \"language\": {\"standard\": \"c++23\", \"compiler_extensions\": false},\n  \"restricted_profile\": {\"exceptions\": false, \"rtti\": false},\n  \"posix_baseline\": \"${LAGHU_POSIX_BASELINE}\",\n  \"capabilities\": {\n${capabilities_json}\n  },\n  \"generator\": \"Ninja\"\n}\n")
  file(WRITE "${LAGHU_PROBE_DIRECTORY}/toolchain-capabilities-v1.json"
"{\n  \"schema_version\": \"toolchain-capabilities-v1\",\n  \"target_os\": \"${CMAKE_SYSTEM_NAME}\",\n  \"compiler\": {\"id\": \"${CMAKE_CXX_COMPILER_ID}\", \"version\": \"${CMAKE_CXX_COMPILER_VERSION}\"},\n  \"standard_library\": {\"id\": \"${LAGHU_STANDARD_LIBRARY_ID}\", \"version\": \"${LAGHU_STANDARD_LIBRARY_VERSION}\"},\n  \"language\": {\"standard\": \"c++23\", \"compiler_extensions\": false},\n  \"restricted_profile\": {\"exceptions\": false, \"rtti\": false},\n  \"posix_baseline\": \"${LAGHU_POSIX_BASELINE}\",\n  \"warning_gates\": [\"${warnings_json}\"],\n  \"capabilities\": {\n${capabilities_json}\n  }\n}\n")
endfunction()

function(laghu_configure_toolchain)
  set(CMAKE_CXX_STANDARD 23 PARENT_SCOPE)
  set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
  set(CMAKE_CXX_EXTENSIONS OFF PARENT_SCOPE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(LAGHU_COMPILER_FAMILY clang CACHE INTERNAL "Laghu compiler family")
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(LAGHU_COMPILER_FAMILY gcc CACHE INTERNAL "Laghu compiler family")
  else()
    laghu_fail(compiler_family "supported_compilers=gcc,clang")
  endif()
  laghu_detect_standard_library()
  set(LAGHU_STANDARD_LIBRARY_ID "${LAGHU_STANDARD_LIBRARY_ID}" CACHE INTERNAL "Laghu standard library")
  set(LAGHU_STANDARD_LIBRARY_VERSION "${LAGHU_STANDARD_LIBRARY_VERSION}" CACHE INTERNAL "Laghu standard library version")
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(LAGHU_POSIX_BASELINE darwin_server_api_exception CACHE INTERNAL "Laghu POSIX baseline")
    message(STATUS "Laghu POSIX baseline: Darwin API exception; concrete server API probes remain required")
  else()
    set(LAGHU_POSIX_BASELINE posix_2008 CACHE INTERNAL "Laghu POSIX baseline")
  endif()
  foreach(capability IN ITEMS if_consteval expected byteswap to_underlying unreachable)
    string(TOUPPER "${capability}" capability_upper)
    laghu_compile_probe("${capability}" "${CMAKE_SOURCE_DIR}/tests/toolchain/probes/${capability}.cpp" FALSE "LAGHU_CAPABILITY_${capability_upper}")
  endforeach()
  foreach(posix_probe IN ITEMS posix_2008 sockets bind_listen_accept nonblocking_fcntl poll close clock_gettime_monotonic)
    if(posix_probe STREQUAL "posix_2008")
      set(posix_result_variable LAGHU_POSIX_2008_PROBE)
    else()
      string(TOUPPER "${posix_probe}" posix_probe_upper)
      set(posix_result_variable "LAGHU_CAPABILITY_${posix_probe_upper}")
    endif()
    laghu_compile_probe("${posix_probe}" "${CMAKE_SOURCE_DIR}/tests/configure/probes/${posix_probe}.cpp" TRUE "${posix_result_variable}")
  endforeach()
  set(LAGHU_CAPABILITY_POSIX_BASELINE true)
  set(LAGHU_EFFECTIVE_WARNING_FLAGS)
  foreach(warning IN ITEMS -Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wformat-security -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wcast-align -Wcast-qual -Wold-style-cast -Woverloaded-virtual -Wnon-virtual-dtor -Wzero-as-null-pointer-constant -Wundef -Wuninitialized)
    laghu_warning_probe("${warning}")
  endforeach()
  if(LAGHU_COMPILER_FAMILY STREQUAL gcc)
    foreach(warning IN ITEMS -Warith-conversion -Wcast-align=strict -Wdangling-pointer=2 -Wformat-overflow=2 -Wformat-truncation=2 -Warray-bounds=2 -Wstringop-overflow=4 -Wduplicated-cond -Wlogical-op -Wuseless-cast)
      laghu_warning_probe("${warning}")
    endforeach()
  else()
    foreach(warning IN ITEMS -Wshadow-all -Wimplicit-int-conversion -Wshorten-64-to-32 -Wdangling -Warray-bounds -Wunsafe-buffer-usage)
      laghu_warning_probe("${warning}")
    endforeach()
  endif()
  set(LAGHU_EFFECTIVE_WARNING_FLAGS "${LAGHU_EFFECTIVE_WARNING_FLAGS}" CACHE INTERNAL "Laghu warning flags")
  laghu_require_no_raw_extensions()
  laghu_require_core_profile_sources()
  laghu_validate_warning_suppressions("${CMAKE_SOURCE_DIR}/tests/warnings/suppressions.tsv")
  laghu_publish_capability_values()
  laghu_write_metadata()
endfunction()

function(laghu_apply_first_party_contract target)
  set_property(TARGET "${target}" PROPERTY CXX_STANDARD 23)
  set_property(TARGET "${target}" PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET "${target}" PROPERTY CXX_EXTENSIONS OFF)
  target_compile_options("${target}" PRIVATE -pedantic-errors -fno-exceptions -fno-rtti ${LAGHU_EFFECTIVE_WARNING_FLAGS})
  get_target_property(effective_options "${target}" COMPILE_OPTIONS)
  list(FIND effective_options -fexceptions enables_exceptions)
  list(FIND effective_options -frtti enables_rtti)
  if(NOT enables_exceptions EQUAL -1 OR NOT enables_rtti EQUAL -1)
    laghu_fail(restricted_profile_flags "target=${target}; exceptions and RTTI may not be re-enabled")
  endif()
endfunction()

function(laghu_add_validation_tests)
  set(expect_compile "${CMAKE_SOURCE_DIR}/cmake/ExpectCompile.cmake")
  foreach(capability IN ITEMS if_consteval expected byteswap to_underlying unreachable)
    add_test(NAME "laghu.toolchain.negative.${capability}" COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/toolchain/negative/${capability}.cpp -DEXPECT_FAIL=ON -DEXPECT_TEXT=laghu\ forced-negative\ capability=${capability} -P "${expect_compile}")
  endforeach()
  add_test(NAME laghu.toolchain.cxx20_rejected COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/toolchain/negative/cxx20-rejected.cpp -DSTANDARD=c++20 -DEXPECT_FAIL=ON -P "${expect_compile}")
  foreach(fixture IN ITEMS throw dynamic-cast)
    add_test(NAME "laghu.profile.compile_negative.${fixture}" COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/profile/compile-negative/${fixture}.cpp "-DFLAGS=-fno-exceptions;-fno-rtti" -DEXPECT_FAIL=ON -P "${expect_compile}")
  endforeach()
  add_test(NAME laghu.profile.value_type COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/profile/positive/value_type.cpp "-DFLAGS=-fno-exceptions;-fno-rtti" -DEXPECT_FAIL=OFF -P "${expect_compile}")
  add_test(NAME laghu.profile.third_party_isolated COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/profile/third-party/throws.cpp -DEXPECT_FAIL=OFF -P "${expect_compile}")
  foreach(fixture IN ITEMS exceptions rtti coroutines futures iostream virtual)
    add_test(NAME "laghu.profile.source_negative.${fixture}" COMMAND "${CMAKE_COMMAND}" -DSOURCE=${CMAKE_SOURCE_DIR}/tests/profile/negative/${fixture}.cpp -DEXPECT_RULE=${fixture} -P "${CMAKE_SOURCE_DIR}/cmake/ExpectProfilePolicy.cmake")
  endforeach()
  add_test(NAME laghu.profile.audit_reenable_exceptions COMMAND "${CMAKE_COMMAND}" -DTRACE=${CMAKE_SOURCE_DIR}/tests/profile/audit/re-enable-exceptions.tsv -DREENABLE=-fexceptions -P "${CMAKE_SOURCE_DIR}/cmake/ExpectProfileAudit.cmake")
  add_test(NAME laghu.profile.audit_reenable_rtti COMMAND "${CMAKE_COMMAND}" -DTRACE=${CMAKE_SOURCE_DIR}/tests/profile/audit/re-enable-rtti.tsv -DREENABLE=-frtti -P "${CMAKE_SOURCE_DIR}/cmake/ExpectProfileAudit.cmake")
  add_test(NAME laghu.warning.first_party_rejected COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/warnings/first-party/conversion.cpp "-DFLAGS=${LAGHU_EFFECTIVE_WARNING_FLAGS}" -DEXPECT_FAIL=ON -P "${expect_compile}")
  add_test(NAME laghu.warning.third_party_isolated COMMAND "${CMAKE_COMMAND}" -DCXX=${CMAKE_CXX_COMPILER} -DCXXFLAGS=${CMAKE_CXX_FLAGS} -DSOURCE=${CMAKE_SOURCE_DIR}/tests/warnings/third-party/conversion.cpp -DEXPECT_FAIL=OFF -P "${expect_compile}")
  foreach(fixture IN ITEMS broad-source invalid-warning missing-reason)
    add_test(NAME "laghu.warning.suppression_negative.${fixture}" COMMAND "${CMAKE_COMMAND}" -DMANIFEST=${CMAKE_SOURCE_DIR}/tests/warnings/negative/${fixture}.tsv -P "${CMAKE_SOURCE_DIR}/cmake/ExpectWarningSuppression.cmake")
  endforeach()
  foreach(mode IN ITEMS all_enabled baseline_unavailable)
    string(REPLACE "_" "-" golden_mode "${mode}")
    add_test(NAME "laghu.capabilities.golden.${mode}"
      COMMAND "${CMAKE_COMMAND}"
        "-DMODE=${mode}"
        "-DMODULE=${CMAKE_SOURCE_DIR}/cmake/LaghuCapabilities.cmake"
        "-DOUTPUT=${CMAKE_BINARY_DIR}/tests/capabilities-${mode}.hpp"
        "-DGOLDEN=${CMAKE_SOURCE_DIR}/tests/configure/golden/capabilities-${golden_mode}.hpp"
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectCapabilityRender.cmake")
  endforeach()
  add_test(NAME laghu.capabilities.parity
    COMMAND "${CMAKE_COMMAND}"
      "-DHEADER=${LAGHU_CAPABILITY_HEADER}"
      "-DCONFIG_JSON=${CMAKE_BINARY_DIR}/config/laghu-config-v1.json"
      "-DPROBE_JSON=${LAGHU_PROBE_DIRECTORY}/toolchain-capabilities-v1.json"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectCapabilityParity.cmake")
  foreach(fixture IN ITEMS positive_leaf positive_protocol positive_top_and_adapter)
    add_test(NAME "laghu.dag.positive.${fixture}"
      COMMAND "${CMAKE_COMMAND}"
        "-DLAGHU_SOURCE=${CMAKE_SOURCE_DIR}"
        "-DSCENARIO=${fixture}"
        -DEXPECT_FAIL=OFF
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectDagConfigure.cmake")
  endforeach()
  foreach(fixture IN ITEMS core_outward os_protocol forbidden_peer cycle adapter_direction generic_utility)
    string(REPLACE "_" "-" fixture_name "${fixture}")
    if(fixture STREQUAL "core_outward")
      set(expected_text "consumer=core provider=config rule=core_has_no_outward_dependencies")
    elseif(fixture STREQUAL "os_protocol")
      set(expected_text "consumer=os provider=protocol rule=os_may_depend_only_on_core")
    elseif(fixture STREQUAL "forbidden_peer")
      set(expected_text "consumer=cache provider=tls rule=edge_not_allowed")
    elseif(fixture STREQUAL "cycle")
      set(expected_text "consumer=core provider=config rule=cycle_forbidden")
    elseif(fixture STREQUAL "adapter_direction")
      set(expected_text "consumer=proxy provider=adapters rule=adapter_direction_inward_only")
    else()
      set(expected_text "subsystem=common rule=no_generic_utility_sink")
    endif()
    add_test(NAME "laghu.dag.negative.${fixture_name}"
      COMMAND "${CMAKE_COMMAND}"
        "-DLAGHU_SOURCE=${CMAKE_SOURCE_DIR}"
        "-DSCENARIO=${fixture}"
        -DEXPECT_FAIL=ON
        "-DEXPECT_TEXT=${expected_text}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ExpectDagConfigure.cmake")
  endforeach()
endfunction()

function(laghu_add_install_layout_test)
  add_test(NAME laghu.build.install_layout
    COMMAND "${CMAKE_COMMAND}"
      "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
      "-DSTAGE_DIRECTORY=${CMAKE_BINARY_DIR}/tests/install-stage"
      -P "${CMAKE_SOURCE_DIR}/cmake/ExpectInstallLayout.cmake")
endfunction()
