# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED CMAKE_CXX_COMPILER OR
    NOT DEFINED CMAKE_CXX_FLAGS OR NOT DEFINED SCENARIO OR NOT DEFINED EXPECT_FAIL)
  message(FATAL_ERROR "ExpectDependencyConfigure requires LAGHU_SOURCE, CMAKE_CXX_COMPILER, CMAKE_CXX_FLAGS, SCENARIO, and EXPECT_FAIL")
endif()

set(mode_suffix "${SOURCE}-${LINK_MODE}-${TLS_PROVIDER}")
set(binary_directory "${CMAKE_CURRENT_BINARY_DIR}/dependency-${SCENARIO}-${mode_suffix}")
set(mode_arguments)
foreach(mode IN ITEMS SOURCE LINK_MODE TLS_PROVIDER)
  if(DEFINED ${mode})
    if("${mode}" STREQUAL "SOURCE")
      list(APPEND mode_arguments "-DLAGHU_DEPENDENCY_SOURCE=${${mode}}")
    elseif("${mode}" STREQUAL "LINK_MODE")
      list(APPEND mode_arguments "-DLAGHU_DEPENDENCY_LINK_MODE=${${mode}}")
    else()
      list(APPEND mode_arguments "-DLAGHU_TLS_PROVIDER=${${mode}}")
    endif()
  endif()
endforeach()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${LAGHU_SOURCE}/tests/dependencies/fixture"
    -B "${binary_directory}"
    "-DLAGHU_SOURCE=${LAGHU_SOURCE}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_CXX_SCAN_FOR_MODULES=ON"
    "-DSCENARIO=${SCENARIO}"
    ${mode_arguments}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_diagnostics)

set(combined "${configure_output}\n${configure_diagnostics}")
if(configure_result EQUAL 0 AND ("${SCENARIO}" STREQUAL "positive_symbol" OR
    "${SCENARIO}" STREQUAL "capability_mismatch" OR "${SCENARIO}" STREQUAL "mode_vendored_yyjson"))
  if("${SCENARIO}" STREQUAL "mode_vendored_yyjson")
    set(build_target laghu_dependency_mode_consumer)
  else()
    set(build_target laghu_dependency_fixture_probe)
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${binary_directory}" --target "${build_target}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_diagnostics)
  string(APPEND combined "\n${build_output}\n${build_diagnostics}")
  set(result "${build_result}")
  if(result EQUAL 0 AND "${SCENARIO}" STREQUAL "mode_vendored_yyjson")
    set(install_directory "${binary_directory}/install")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --install "${binary_directory}" --prefix "${install_directory}"
      RESULT_VARIABLE install_result
      OUTPUT_VARIABLE install_output
      ERROR_VARIABLE install_diagnostics)
    string(APPEND combined "\n${install_output}\n${install_diagnostics}")
    if(NOT install_result EQUAL 0)
      set(result "${install_result}")
    else()
      file(GLOB_RECURSE installed_paths LIST_DIRECTORIES FALSE "${install_directory}/*")
      if(installed_paths)
        message(FATAL_ERROR
          "Laghu dependency fixture installed external files: scenario=${SCENARIO}; files=${installed_paths}")
      endif()
    endif()
  endif()
else()
  set(result "${configure_result}")
endif()

if(EXPECT_FAIL AND result EQUAL 0)
  message(FATAL_ERROR "Laghu dependency fixture unexpectedly passed: scenario=${SCENARIO}")
endif()
if(NOT EXPECT_FAIL AND NOT result EQUAL 0)
  message(FATAL_ERROR "Laghu dependency fixture failed: scenario=${SCENARIO}\n${combined}")
endif()
if(NOT EXPECT_TEXT STREQUAL "")
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_combined "${combined}")
  string(REGEX REPLACE "[ \t\r\n]+" " " normalized_expected "${EXPECT_TEXT}")
  string(FIND "${normalized_combined}" "${normalized_expected}" text_offset)
  if(text_offset EQUAL -1)
    message(FATAL_ERROR "Laghu dependency fixture diagnostic missing: scenario=${SCENARIO}; expected=${EXPECT_TEXT}\n${combined}")
  endif()
endif()
