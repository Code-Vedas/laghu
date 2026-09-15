# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SCRIPT OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED SOURCE_DIRECTORY OR
    NOT DEFINED EXPECTED_FEATURE_COUNT OR NOT DEFINED EXPECTED_DEPENDENCY_COUNT OR
    NOT DEFINED EXPECTED_SANITIZER_PROFILE)
  message(FATAL_ERROR "Laghu benchmark expectation requires SCRIPT BUILD_DIRECTORY SOURCE_DIRECTORY EXPECTED_FEATURE_COUNT EXPECTED_DEPENDENCY_COUNT and EXPECTED_SANITIZER_PROFILE")
endif()

function(laghu_benchmark_run output warmup)
  execute_process(
    COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --workload core-foundation --warmup "${warmup}" --intervals 5
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Laghu benchmark expectation failed: positive=${standard_output}${diagnostics}")
  endif()
  set(${output} "${standard_output}" PARENT_SCOPE)
endfunction()

laghu_benchmark_run(first 1)
laghu_benchmark_run(second 1)
laghu_benchmark_run(no_warmup 0)
laghu_benchmark_run(extended_warmup 7)

foreach(output IN ITEMS "${first}" "${second}")
  if(NOT output MATCHES "^\\{\\\"schema_version\\\":\\\"laghu-benchmark-v1\\\"")
    message(FATAL_ERROR "Laghu benchmark expectation failed: canonical_schema_prefix_missing")
  endif()
  string(FIND "${output}" "${SOURCE_DIRECTORY}" source_path)
  string(FIND "${output}" "${BUILD_DIRECTORY}" build_path)
  if(NOT source_path EQUAL -1 OR NOT build_path EQUAL -1)
    message(FATAL_ERROR "Laghu benchmark expectation failed: path_leakage")
  endif()
  string(JSON schema GET "${output}" schema_version)
  string(JSON workload GET "${output}" workload)
  string(JSON build_id GET "${output}" build build_id)
  string(JSON feature_count LENGTH "${output}" build features)
  string(JSON dependency_count LENGTH "${output}" build dependencies)
  string(JSON interval_count GET "${output}" parameters intervals)
  string(JSON warmup_count GET "${output}" parameters warmup)
  string(JSON operations GET "${output}" parameters operations_per_interval)
  string(JSON p50 GET "${output}" metrics latency_ns_per_interval p50)
  string(JSON p95 GET "${output}" metrics latency_ns_per_interval p95)
  string(JSON p99 GET "${output}" metrics latency_ns_per_interval p99)
  string(JSON p999 GET "${output}" metrics latency_ns_per_interval p99_9)
  string(JSON sample_count LENGTH "${output}" metrics latency_ns_per_interval samples_ns)
  string(JSON profile GET "${output}" build profile)
  string(JSON sanitizer_profile GET "${output}" build sanitizer_profile)
  string(JSON standard_library_id GET "${output}" build standard_library id)
  string(JSON standard_library_version GET "${output}" build standard_library version)
  string(JSON allocation_instrumented GET "${output}" metrics allocation_count instrumented)
  string(JSON allocation_count GET "${output}" metrics allocation_count value)
  string(JSON syscall_instrumented GET "${output}" metrics laghu_syscall_count instrumented)
  string(JSON syscall_count GET "${output}" metrics laghu_syscall_count value)
  if(NOT schema STREQUAL "laghu-benchmark-v1" OR NOT workload STREQUAL "core-foundation" OR
      build_id STREQUAL "" OR NOT feature_count EQUAL EXPECTED_FEATURE_COUNT OR
      NOT dependency_count EQUAL EXPECTED_DEPENDENCY_COUNT OR
      NOT interval_count EQUAL 5 OR NOT warmup_count EQUAL 1 OR NOT operations EQUAL 4096 OR
      NOT sample_count EQUAL 5 OR
      NOT profile STREQUAL "MINIMAL" OR NOT sanitizer_profile STREQUAL EXPECTED_SANITIZER_PROFILE OR
      standard_library_id STREQUAL "" OR standard_library_version STREQUAL "" OR
      p50 GREATER p95 OR p95 GREATER p99 OR p99 GREATER p999 OR
      allocation_instrumented OR syscall_instrumented OR
      NOT allocation_count EQUAL 0 OR NOT syscall_count EQUAL 0)
    message(FATAL_ERROR "Laghu benchmark expectation failed: schema_or_metric_invalid")
  endif()
  string(JSON cpu_description_status GET "${output}" cpu description status)
  foreach(status IN ITEMS "${cpu_description_status}")
    if(NOT status STREQUAL "available" AND NOT status STREQUAL "unavailable")
      message(FATAL_ERROR "Laghu benchmark expectation failed: host_metric_status_invalid")
    endif()
  endforeach()
  foreach(metric IN ITEMS cpu_time_ns peak_rss_bytes throughput_operations_per_second)
    string(JSON status GET "${output}" metrics ${metric} status)
    if(status STREQUAL "available")
      if(metric STREQUAL "cpu_time_ns")
        set(samples_key samples_ns)
      elseif(metric STREQUAL "peak_rss_bytes")
        set(samples_key samples_bytes)
      else()
        set(samples_key samples_operations_per_second)
      endif()
      string(JSON metric_sample_count LENGTH "${output}" metrics ${metric} ${samples_key})
      if(NOT metric_sample_count EQUAL 5)
        message(FATAL_ERROR "Laghu benchmark expectation failed: interval_sample_count_invalid metric=${metric}")
      endif()
    elseif(NOT status STREQUAL "unavailable")
      message(FATAL_ERROR "Laghu benchmark expectation failed: host_metric_status_invalid metric=${metric}")
    endif()
  endforeach()
  foreach(metric IN ITEMS allocation_count laghu_syscall_count)
    string(JSON status GET "${output}" metrics ${metric} status)
    string(JSON reason GET "${output}" metrics ${metric} reason)
    if(NOT status STREQUAL "unavailable" OR reason STREQUAL "")
      message(FATAL_ERROR "Laghu benchmark expectation failed: uninstrumented_counter_invalid metric=${metric}")
    endif()
  endforeach()
endforeach()

string(JSON first_build GET "${first}" build)
string(JSON second_build GET "${second}" build)
string(JSON first_parameters GET "${first}" parameters)
string(JSON second_parameters GET "${second}" parameters)
if(NOT first_build STREQUAL second_build OR NOT first_parameters STREQUAL second_parameters)
  message(FATAL_ERROR "Laghu benchmark expectation failed: deterministic_identity_or_parameters_invalid")
endif()

foreach(metric IN ITEMS allocation_count laghu_syscall_count)
  string(JSON no_warmup_instrumented GET "${no_warmup}" metrics ${metric} instrumented)
  string(JSON extended_warmup_instrumented GET "${extended_warmup}" metrics ${metric} instrumented)
  string(JSON no_warmup_value GET "${no_warmup}" metrics ${metric} value)
  string(JSON extended_warmup_value GET "${extended_warmup}" metrics ${metric} value)
  if(NOT no_warmup_instrumented STREQUAL extended_warmup_instrumented OR
      NOT no_warmup_value EQUAL extended_warmup_value)
    message(FATAL_ERROR "Laghu benchmark expectation failed: warmup_counter_leak metric=${metric}")
  endif()
endforeach()

foreach(case IN ITEMS missing-workload invalid-workload zero-intervals zero-padded-intervals
    zero-tripled-intervals missing-build missing-directory)
  if(case STREQUAL "missing-workload")
    set(arguments --build "${BUILD_DIRECTORY}" --warmup 1 --intervals 1)
  elseif(case STREQUAL "invalid-workload")
    set(arguments --build "${BUILD_DIRECTORY}" --workload invalid --warmup 1 --intervals 1)
  elseif(case STREQUAL "zero-intervals")
    set(arguments --build "${BUILD_DIRECTORY}/missing" --workload core-foundation --warmup 1 --intervals 0)
  elseif(case STREQUAL "zero-padded-intervals")
    set(arguments --build "${BUILD_DIRECTORY}/missing" --workload core-foundation --warmup 1 --intervals 00)
  elseif(case STREQUAL "zero-tripled-intervals")
    set(arguments --build "${BUILD_DIRECTORY}/missing" --workload core-foundation --warmup 1 --intervals 000)
  elseif(case STREQUAL "missing-directory")
    set(arguments --build "${BUILD_DIRECTORY}/missing" --workload core-foundation --warmup 1 --intervals 1)
  else()
    set(arguments --workload core-foundation --warmup 1 --intervals 1)
  endif()
  execute_process(COMMAND "${SCRIPT}" ${arguments}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics)
  if(case STREQUAL "missing-directory")
    set(expected_exit 66)
  else()
    set(expected_exit 64)
  endif()
  if(NOT result EQUAL expected_exit)
    message(FATAL_ERROR "Laghu benchmark expectation failed: case=${case}; expected_exit=${expected_exit}; actual_exit=${result}; output=${output}${diagnostics}")
  endif()
endforeach()

execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --workload core-foundation --warmup 1 --intervals 001
  RESULT_VARIABLE padded_result
  OUTPUT_VARIABLE padded_output
  ERROR_VARIABLE padded_diagnostics)
if(NOT padded_result EQUAL 0)
  message(FATAL_ERROR "Laghu benchmark expectation failed: nonzero_padded_intervals=${padded_output}${padded_diagnostics}")
endif()
string(JSON padded_interval_count GET "${padded_output}" parameters intervals)
if(NOT padded_interval_count EQUAL 1)
  message(FATAL_ERROR "Laghu benchmark expectation failed: nonzero_padded_intervals_invalid")
endif()
