# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SCRIPT OR NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED SOURCE_DIRECTORY)
  message(FATAL_ERROR "Laghu benchmark evaluator expectation requires SCRIPT BUILD_DIRECTORY and SOURCE_DIRECTORY")
endif()

set(work "${BUILD_DIRECTORY}/tests/benchmark-evaluator")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(laghu_write_artifact path build_id cpu samples)
  set(intervals 10)
  set(document
"{\"schema_version\":\"laghu-benchmark-v1\",\"build\":{\"build_id\":\"${build_id}\",\"compiler\":{\"id\":\"FixtureCxx\",\"version\":\"1\"},\"dependencies\":[],\"features\":[\"core\"],\"profile\":\"MINIMAL\",\"sanitizer_profile\":\"NONE\",\"standard_library\":{\"id\":\"fixture-stdlib\",\"version\":\"1\"},\"target\":{\"architecture\":\"fixture64\",\"os\":\"FixtureOS\"}},\"cpu\":{\"description\":{\"status\":\"available\",\"value\":\"${cpu}\"}},\"metrics\":{\"allocation_count\":{\"instrumented\":false,\"value\":0},\"cpu_time_ns\":{\"status\":\"available\",\"value\":1},\"laghu_syscall_count\":{\"instrumented\":false,\"value\":0},\"latency_ns_per_interval\":{\"p50\":100,\"p95\":100,\"p99\":100,\"p99_9\":100,\"samples_ns\":[${samples}]},\"peak_rss_bytes\":{\"status\":\"available\",\"value\":1},\"throughput_operations_per_second\":{\"status\":\"available\",\"value\":1}},\"parameters\":{\"intervals\":${intervals},\"operations_per_interval\":4096,\"warmup\":1},\"workload\":\"core-foundation\",\"workload_checksum\":1}\n")
  file(WRITE "${path}" "${document}")
endfunction()

set(baseline_samples "100,100,100,100,100,100,100,100,100,100")
set(clear_samples "120,120,120,120,120,120,120,120,120,120")
set(higher_regression_samples "80,80,80,80,80,80,80,80,80,80")
set(noise_samples "104,104,104,104,104,104,104,104,104,104")
set(short_samples "100,100,100,100,100,100,100,100,100")
laghu_write_artifact("${work}/baseline.json" baseline fixture-cpu "${baseline_samples}")
laghu_write_artifact("${work}/clear.json" candidate-clear fixture-cpu "${clear_samples}")
laghu_write_artifact("${work}/higher-regression.json" candidate-higher fixture-cpu "${higher_regression_samples}")
laghu_write_artifact("${work}/noise.json" candidate-noise fixture-cpu "${noise_samples}")
laghu_write_artifact("${work}/incompatible.json" candidate-incompatible fixture-other-cpu "${baseline_samples}")
laghu_write_artifact("${work}/short.json" candidate-short fixture-cpu "${short_samples}")

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
file(WRITE "${manifest}"
  "# workload\tmetric\tdirection\tallowed_regression_percent\tnoise_band_percent\treference_environment\tmode\ncore-foundation\tlatency_ns_per_interval\tlower\t5\t1\t${environment}\thard\n")

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

laghu_evaluator_run("${work}/clear.json" 1 "\"lower\":200000" clear_output)
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

file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\tlower\t5\t1\t${environment}\tadvisory\n")
laghu_evaluator_run("${work}/clear.json" 0 "advisory_regression" advisory_output)

file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\thigher\t5\t1\t${environment}\thard\n")
laghu_evaluator_run("${work}/higher-regression.json" 1 "\"lower\":200000" higher_output)

file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\tlower\t5\t1\t${environment}\thard\n")
laghu_evaluator_run("${work}/incompatible.json" 65 "compatibility=hardware" incompatible_output)
laghu_evaluator_run("${work}/short.json" 65 "artifact=candidate; field=sample_count" short_output)

file(READ "${work}/baseline.json" missing_document)
string(REGEX REPLACE ",\"samples_ns\":\\[[0-9,]+\\]" "" missing_document "${missing_document}")
file(WRITE "${work}/missing.json" "${missing_document}")
laghu_evaluator_run("${work}/missing.json" 65 "artifact=candidate; field=metrics" missing_output)

file(READ "${work}/baseline.json" incomplete_metrics_document)
string(REPLACE "\"cpu_time_ns\":{\"status\":\"available\",\"value\":1}," "" incomplete_metrics_document "${incomplete_metrics_document}")
file(WRITE "${work}/incomplete-metrics.json" "${incomplete_metrics_document}")
laghu_evaluator_run("${work}/incomplete-metrics.json" 65 "artifact=candidate; field=metrics" incomplete_metrics_output)

file(WRITE "${manifest}"
  "# fixture\ncore-foundation\tlatency_ns_per_interval\tinvalid\t5\t1\t${environment}\thard\n")
laghu_evaluator_run("${work}/noise.json" 65 "manifest=direction" malformed_output)

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
