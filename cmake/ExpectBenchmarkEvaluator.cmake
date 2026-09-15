# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SCRIPT OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED SOURCE_DIRECTORY)
  message(FATAL_ERROR "Laghu benchmark evaluator expectation requires SCRIPT BUILD_DIRECTORY and SOURCE_DIRECTORY")
endif()

set(work "${BUILD_DIRECTORY}/tests/benchmark-evaluator")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(laghu_write_artifact path build_id cpu latency throughput cpu_time rss allocation syscall)
  set(document
"{\"schema_version\":\"laghu-benchmark-v1\",\"build\":{\"build_id\":\"${build_id}\",\"compiler\":{\"id\":\"FixtureCxx\",\"version\":\"1\"},\"dependencies\":[],\"features\":[\"core\"],\"profile\":\"MINIMAL\",\"sanitizer_profile\":\"NONE\",\"standard_library\":{\"id\":\"fixture-stdlib\",\"version\":\"1\"},\"target\":{\"architecture\":\"fixture64\",\"os\":\"FixtureOS\"}},\"cpu\":{\"description\":{\"status\":\"available\",\"value\":\"${cpu}\"}},\"metrics\":{\"allocation_count\":{\"instrumented\":true,\"value\":100,\"status\":\"available\",\"samples_count\":[${allocation}]},\"cpu_time_ns\":{\"status\":\"available\",\"value\":100,\"samples_ns\":[${cpu_time}]},\"laghu_syscall_count\":{\"instrumented\":true,\"value\":100,\"status\":\"available\",\"samples_count\":[${syscall}]},\"latency_ns_per_interval\":{\"p50\":100,\"p95\":100,\"p99\":100,\"p99_9\":100,\"samples_ns\":[${latency}]},\"peak_rss_bytes\":{\"status\":\"available\",\"value\":100,\"samples_bytes\":[${rss}]},\"throughput_operations_per_second\":{\"status\":\"available\",\"value\":100,\"samples_operations_per_second\":[${throughput}]}},\"parameters\":{\"intervals\":10,\"operations_per_interval\":4096,\"warmup\":1},\"workload\":\"core-foundation\",\"workload_checksum\":1}\n")
  file(WRITE "${path}" "${document}")
endfunction()

set(samples_100 "100,100,100,100,100,100,100,100,100,100")
set(samples_120 "120,120,120,120,120,120,120,120,120,120")
set(samples_104 "104,104,104,104,104,104,104,104,104,104")
set(samples_080 "80,80,80,80,80,80,80,80,80,80")
set(samples_000 "0,0,0,0,0,0,0,0,0,0")
laghu_write_artifact("${work}/baseline.json" baseline fixture-cpu
  "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}")
laghu_write_artifact("${work}/lower-regression.json" candidate-lower fixture-cpu
  "${samples_120}" "${samples_120}" "${samples_120}" "${samples_120}" "${samples_120}" "${samples_120}")
laghu_write_artifact("${work}/higher-regression.json" candidate-higher fixture-cpu
  "${samples_080}" "${samples_080}" "${samples_080}" "${samples_080}" "${samples_080}" "${samples_080}")
laghu_write_artifact("${work}/noise.json" candidate-noise fixture-cpu
  "${samples_104}" "${samples_104}" "${samples_104}" "${samples_104}" "${samples_104}" "${samples_104}")
laghu_write_artifact("${work}/incompatible.json" candidate-incompatible fixture-other-cpu
  "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}" "${samples_100}")
laghu_write_artifact("${work}/zero.json" candidate-zero fixture-cpu
  "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}")
laghu_write_artifact("${work}/zero-baseline.json" baseline-zero fixture-cpu
  "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}" "${samples_000}")

execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --environment "${work}/baseline.json"
  RESULT_VARIABLE environment_result
  OUTPUT_VARIABLE environment
  ERROR_VARIABLE environment_diagnostics)
string(STRIP "${environment}" environment)
string(LENGTH "${environment}" environment_length)
if(NOT environment_result EQUAL 0 OR NOT environment_length EQUAL 16 OR
    NOT environment MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: environment=${environment}${environment_diagnostics}")
endif()

set(manifest "${work}/thresholds.tsv")
function(laghu_write_manifest metric direction mode)
  file(WRITE "${manifest}"
    "# workload\tmetric\tdirection\tallowed_regression_percent\tnoise_band_percent\treference_environment\tmode\ncore-foundation\t${metric}\t${direction}\t5\t1\t${environment}\t${mode}\n")
endfunction()

function(laghu_evaluator_run candidate expected_exit expected_text output)
  execute_process(
    COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --baseline "${work}/baseline.json"
      --candidate "${candidate}" --manifest "${manifest}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL expected_exit OR NOT "${standard_output}${diagnostics}" MATCHES "${expected_text}")
    message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: candidate=${candidate}; expected_exit=${expected_exit}; actual_exit=${result}; output=${standard_output}${diagnostics}")
  endif()
  set(${output} "${standard_output}" PARENT_SCOPE)
endfunction()

foreach(metric IN ITEMS latency_ns_per_interval cpu_time_ns peak_rss_bytes allocation_count laghu_syscall_count)
  laghu_write_manifest("${metric}" lower hard)
  laghu_evaluator_run("${work}/lower-regression.json" 1 "\"metric\":\"${metric}\"" lower_output)
  laghu_write_manifest("${metric}" higher hard)
  laghu_evaluator_run("${work}/lower-regression.json" 0 "\"status\":\"pass\"" higher_output)
endforeach()
laghu_write_manifest(throughput_operations_per_second higher hard)
laghu_evaluator_run("${work}/higher-regression.json" 1 "\"metric\":\"throughput_operations_per_second\"" throughput_output)
laghu_write_manifest(throughput_operations_per_second lower hard)
laghu_evaluator_run("${work}/higher-regression.json" 0 "\"status\":\"pass\"" throughput_lower_output)

laghu_write_manifest(latency_ns_per_interval lower hard)
laghu_evaluator_run("${work}/noise.json" 0 "\"status\":\"pass\"" noise_first)
laghu_evaluator_run("${work}/noise.json" 0 "\"status\":\"pass\"" noise_second)
if(NOT noise_first STREQUAL noise_second)
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: deterministic_bootstrap_output")
endif()
string(FIND "${noise_first}" "${SOURCE_DIRECTORY}" source_path)
string(FIND "${noise_first}" "${BUILD_DIRECTORY}" build_path)
if(NOT source_path EQUAL -1 OR NOT build_path EQUAL -1)
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: path_leakage")
endif()

laghu_write_manifest(latency_ns_per_interval lower advisory)
laghu_evaluator_run("${work}/lower-regression.json" 0 "advisory_regression" advisory_output)

laghu_write_manifest(latency_ns_per_interval lower hard)
laghu_evaluator_run("${work}/incompatible.json" 65 "compatibility=hardware" incompatible_output)

file(READ "${work}/baseline.json" unavailable_cpu_document)
string(REPLACE "\"cpu_time_ns\":{\"status\":\"available\",\"value\":100,\"samples_ns\":[${samples_100}]}"
  "\"cpu_time_ns\":{\"status\":\"unavailable\",\"reason\":\"fixture_cpu_time_unavailable\"}"
  unavailable_cpu_document "${unavailable_cpu_document}")
file(WRITE "${work}/unavailable-cpu-time.json" "${unavailable_cpu_document}")
laghu_write_manifest(cpu_time_ns lower hard)
laghu_evaluator_run("${work}/unavailable-cpu-time.json" 65 "metric=cpu_time_ns; reason=candidate_unavailable" unavailable_metric_output)

file(READ "${work}/baseline.json" unavailable_counter_document)
string(REPLACE "\"allocation_count\":{\"instrumented\":true,\"value\":100,\"status\":\"available\",\"samples_count\":[${samples_100}]}"
  "\"allocation_count\":{\"instrumented\":false,\"value\":0,\"status\":\"unavailable\",\"reason\":\"fixture_allocation_unavailable\"}"
  unavailable_counter_document "${unavailable_counter_document}")
file(WRITE "${work}/unavailable-counter.json" "${unavailable_counter_document}")
laghu_write_manifest(allocation_count lower hard)
laghu_evaluator_run("${work}/unavailable-counter.json" 65 "metric=allocation_count; reason=candidate_unavailable" unavailable_counter_output)

file(READ "${work}/baseline.json" unavailable_identity_document)
string(REPLACE "\"cpu\":{\"description\":{\"status\":\"available\",\"value\":\"fixture-cpu\"}}"
  "\"cpu\":{\"description\":{\"status\":\"unavailable\",\"reason\":\"fixture_cpu_identity_unavailable\"}}"
  unavailable_identity_document "${unavailable_identity_document}")
file(WRITE "${work}/unavailable-identity.json" "${unavailable_identity_document}")
execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --environment "${work}/unavailable-identity.json"
  RESULT_VARIABLE unavailable_environment_result
  OUTPUT_VARIABLE unavailable_environment_output
  ERROR_VARIABLE unavailable_environment_diagnostics)
if(NOT unavailable_environment_result EQUAL 65 OR
    NOT "${unavailable_environment_output}${unavailable_environment_diagnostics}" MATCHES "environment=hardware_identity_unavailable")
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: unavailable_environment=${unavailable_environment_output}${unavailable_environment_diagnostics}")
endif()
laghu_write_manifest(latency_ns_per_interval lower hard)
laghu_evaluator_run("${work}/unavailable-identity.json" 65 "compatibility=hardware_identity_unavailable" unavailable_identity_output)

laghu_evaluator_run("${work}/zero.json" 0 "\"status\":\"pass\"" zero_candidate_lower_output)
laghu_write_manifest(latency_ns_per_interval higher hard)
laghu_evaluator_run("${work}/zero.json" 1 "\"lower\":1000000" zero_candidate_higher_output)
file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\tlower\t5\t1\t${environment}\thard\n")
execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --baseline "${work}/zero-baseline.json"
    --candidate "${work}/zero.json" --manifest "${manifest}"
  RESULT_VARIABLE zero_equal_result
  OUTPUT_VARIABLE zero_equal_output
  ERROR_VARIABLE zero_equal_diagnostics)
if(NOT zero_equal_result EQUAL 0 OR NOT "${zero_equal_output}${zero_equal_diagnostics}" MATCHES "\"status\":\"pass\"")
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: zero_equal=${zero_equal_output}${zero_equal_diagnostics}")
endif()
execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --baseline "${work}/zero-baseline.json"
    --candidate "${work}/baseline.json" --manifest "${manifest}"
  RESULT_VARIABLE zero_baseline_result
  OUTPUT_VARIABLE zero_baseline_output
  ERROR_VARIABLE zero_baseline_diagnostics)
if(NOT zero_baseline_result EQUAL 65 OR
    NOT "${zero_baseline_output}${zero_baseline_diagnostics}" MATCHES "zero_baseline_undefined")
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: zero_baseline=${zero_baseline_output}${zero_baseline_diagnostics}")
endif()

file(READ "${work}/baseline.json" missing_document)
string(REPLACE ",\"samples_operations_per_second\":[${samples_100}]" "" missing_document "${missing_document}")
file(WRITE "${work}/missing.json" "${missing_document}")
laghu_write_manifest(throughput_operations_per_second higher hard)
laghu_evaluator_run("${work}/missing.json" 65 "artifact=candidate; field=metrics" missing_output)

file(READ "${work}/baseline.json" short_document)
string(REPLACE "\"intervals\":10" "\"intervals\":9" short_document "${short_document}")
file(WRITE "${work}/short.json" "${short_document}")
laghu_evaluator_run("${work}/short.json" 65 "artifact=candidate; field=parameters" short_output)

file(WRITE "${manifest}"
  "# fixture\nfuture-workload\tunsupported_metric\tlower\t5\t1\t${environment}\thard\n")
laghu_evaluator_run("${work}/noise.json" 65 "manifest=metric" malformed_output)
file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\tinvalid\t5\t1\t${environment}\thard\n")
laghu_evaluator_run("${work}/noise.json" 65 "manifest=direction" malformed_direction_output)

execute_process(
  COMMAND "${SCRIPT}" --build "${BUILD_DIRECTORY}" --baseline "${work}/baseline.json"
  RESULT_VARIABLE usage_result
  OUTPUT_VARIABLE usage_output
  ERROR_VARIABLE usage_diagnostics)
if(NOT usage_result EQUAL 64 OR NOT "${usage_output}${usage_diagnostics}" MATCHES "usage:")
  message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: wrapper_usage=${usage_output}${usage_diagnostics}")
endif()

file(STRINGS "${SOURCE_DIRECTORY}/bench/regression-gates.tsv" production_lines)
foreach(line IN LISTS production_lines)
  if(NOT line STREQUAL "" AND NOT line MATCHES "^#")
    message(FATAL_ERROR "Laghu benchmark evaluator expectation failed: production_threshold_present line=${line}")
  endif()
endforeach()
