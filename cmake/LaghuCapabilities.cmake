# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

# This list is the stable C++ capability schema.  Probe code records values
# into the corresponding LAGHU_CAPABILITY_<UPPERCASE_KEY> variables; both
# configure metadata documents render those same variables.
set(LAGHU_CAPABILITY_KEYS
  if_consteval
  expected
  byteswap
  to_underlying
  unreachable
  posix_baseline)

# Metadata retains the individual server API probe results introduced by the
# configure contract.  The generated header intentionally exposes only the
# stable aggregate POSIX baseline constant above.
set(LAGHU_METADATA_CAPABILITY_KEYS
  ${LAGHU_CAPABILITY_KEYS}
  sockets
  bind_listen_accept
  nonblocking_fcntl
  poll
  close
  clock_gettime_monotonic)

function(laghu_capability_variable key output)
  string(TOUPPER "${key}" key_upper)
  set(${output} "LAGHU_CAPABILITY_${key_upper}" PARENT_SCOPE)
endfunction()

function(laghu_validate_capability_values)
  foreach(key IN LISTS ARGN)
    laghu_capability_variable("${key}" variable)
    if(NOT DEFINED ${variable})
      message(FATAL_ERROR "Laghu capability rendering failed: missing_value=${key}")
    endif()
    if(NOT "${${variable}}" STREQUAL "true" AND NOT "${${variable}}" STREQUAL "false")
      message(FATAL_ERROR
        "Laghu capability rendering failed: invalid_value=${key}; expected=true-or-false")
    endif()
  endforeach()
endfunction()

function(laghu_capability_json_members output)
  laghu_validate_capability_values(${LAGHU_METADATA_CAPABILITY_KEYS})
  set(members)
  foreach(key IN LISTS LAGHU_METADATA_CAPABILITY_KEYS)
    laghu_capability_variable("${key}" variable)
    list(APPEND members "    \"${key}\": ${${variable}}")
  endforeach()
  list(JOIN members ",\n" rendered_members)
  set(${output} "${rendered_members}" PARENT_SCOPE)
endfunction()

function(laghu_publish_capability_values)
  laghu_validate_capability_values(${LAGHU_METADATA_CAPABILITY_KEYS})
  foreach(key IN LISTS LAGHU_METADATA_CAPABILITY_KEYS)
    laghu_capability_variable("${key}" variable)
    set(${variable} "${${variable}}" CACHE INTERNAL "Laghu capability ${key}")
  endforeach()
endfunction()

function(laghu_generate_capability_header output)
  laghu_validate_capability_values(${LAGHU_CAPABILITY_KEYS})
  get_filename_component(output_directory "${output}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")

  set(contents "// SPDX-License-Identifier: AGPL-3.0-only\n#pragma once\n\nnamespace laghu::capability {\n")
  foreach(key IN LISTS LAGHU_CAPABILITY_KEYS)
    laghu_capability_variable("${key}" variable)
    string(APPEND contents "inline constexpr bool has_${key} = ${${variable}};\n")
  endforeach()
  string(APPEND contents "}  // namespace laghu::capability\n")
  file(WRITE "${output}" "${contents}")
endfunction()

function(laghu_configure_capability_header)
  set(header "${CMAKE_BINARY_DIR}/generated/laghu/capabilities.hpp")
  set(repeated_header "${CMAKE_BINARY_DIR}/generated/laghu/capabilities.repeated.hpp")
  laghu_generate_capability_header("${header}")
  laghu_generate_capability_header("${repeated_header}")
  file(SHA256 "${header}" header_digest)
  file(SHA256 "${repeated_header}" repeated_header_digest)
  file(REMOVE "${repeated_header}")
  if(NOT header_digest STREQUAL repeated_header_digest)
    message(FATAL_ERROR "Laghu capability rendering failed: output is not deterministic")
  endif()
  set(LAGHU_CAPABILITY_HEADER "${header}" CACHE INTERNAL "Laghu generated capability header")
endfunction()

function(laghu_add_capability_header_parity_target)
  configure_file(
    "${CMAKE_SOURCE_DIR}/tests/configure/CapabilityHeaderParity.cpp.in"
    "${CMAKE_BINARY_DIR}/tests/capability-header-parity.cpp"
    @ONLY)
  add_library(laghu_capability_header_parity OBJECT
    "${CMAKE_BINARY_DIR}/tests/capability-header-parity.cpp")
  target_include_directories(laghu_capability_header_parity PRIVATE
    "${CMAKE_BINARY_DIR}/generated")
  laghu_apply_first_party_contract(laghu_capability_header_parity)
endfunction()
