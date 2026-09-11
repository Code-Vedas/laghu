# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED BUILD_DIRECTORY OR NOT DEFINED ARCHIVE OR NOT DEFINED EXECUTABLE OR NOT DEFINED FUZZER OR
    NOT DEFINED FUZZER_TARGET OR NOT DEFINED NM OR NOT DEFINED STAGE_DIRECTORY)
  message(FATAL_ERROR "Laghu fuzz release exclusion requires BUILD_DIRECTORY ARCHIVE EXECUTABLE FUZZER FUZZER_TARGET NM and STAGE_DIRECTORY")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}" --target
    laghu_core laghu "${FUZZER_TARGET}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_diagnostics)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz release exclusion failed: build=${build_output}${build_diagnostics}")
endif()

execute_process(COMMAND "${NM}" -g "${FUZZER}"
  RESULT_VARIABLE fuzzer_result
  OUTPUT_VARIABLE fuzzer_symbols
  ERROR_VARIABLE fuzzer_diagnostics)
if(NOT fuzzer_result EQUAL 0 OR NOT fuzzer_symbols MATCHES "LLVMFuzzerTestOneInput")
  message(FATAL_ERROR "Laghu fuzz release exclusion failed: fuzzer_entrypoint=missing; nm=${fuzzer_diagnostics}")
endif()

file(REMOVE_RECURSE "${STAGE_DIRECTORY}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${STAGE_DIRECTORY}"
    "${CMAKE_COMMAND}" --install "${BUILD_DIRECTORY}" --prefix /opt/laghu
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_diagnostics)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "Laghu fuzz release exclusion failed: install=${install_output}${install_diagnostics}")
endif()

set(installed_archive "${STAGE_DIRECTORY}/opt/laghu/lib/laghu/liblaghu_core.a")
set(installed_executable "${STAGE_DIRECTORY}/opt/laghu/bin/laghu")
foreach(artifact IN ITEMS "${ARCHIVE}" "${EXECUTABLE}" "${installed_archive}" "${installed_executable}")
  execute_process(COMMAND "${NM}" -g "${artifact}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE diagnostics)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Laghu fuzz release exclusion failed: artifact=${artifact}; nm=${diagnostics}")
  endif()
  if(symbols MATCHES "LLVMFuzzer(TestOneInput|)|__sanitizer|__asan|__ubsan")
    message(FATAL_ERROR "Laghu fuzz release exclusion failed: artifact=${artifact}; fuzz_symbol=present")
  endif()
endforeach()
