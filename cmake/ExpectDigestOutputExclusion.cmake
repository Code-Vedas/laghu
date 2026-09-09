# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED SOURCE OR NOT DEFINED EXECUTABLE)
  message(FATAL_ERROR "Laghu digest output exclusion test requires source and executable")
endif()

file(GLOB_RECURSE output_sources LIST_DIRECTORIES FALSE
  "${SOURCE}/src/cli/*.cpp"
  "${SOURCE}/src/control/*.cpp"
  "${SOURCE}/src/observability/*.cpp")
list(APPEND output_sources "${SOURCE}/cmake/LaghuBuildIdentity.cmake")
foreach(output_source IN LISTS output_sources)
  file(READ "${output_source}" output_text)
  if(output_text MATCHES "SecretFingerprint|FingerprintKeyId|secret_fingerprint")
    message(FATAL_ERROR "Laghu digest output exclusion test found a fingerprint in ${output_source}")
  endif()
endforeach()

foreach(version_flag IN ITEMS "" --verbose --json)
  if(version_flag STREQUAL "")
    execute_process(
      COMMAND "${EXECUTABLE}" version
      RESULT_VARIABLE version_result
      OUTPUT_VARIABLE version_output
      ERROR_VARIABLE version_error)
  else()
    execute_process(
      COMMAND "${EXECUTABLE}" version "${version_flag}"
      RESULT_VARIABLE version_result
      OUTPUT_VARIABLE version_output
      ERROR_VARIABLE version_error)
  endif()
  if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Laghu digest output exclusion test could not run version output: ${version_error}")
  endif()
  string(TOLOWER "${version_output}" normalized_version_output)
  if(normalized_version_output MATCHES "secretfingerprint|fingerprintkeyid")
    message(FATAL_ERROR "Laghu digest output exclusion test found a fingerprint in version output")
  endif()
endforeach()
