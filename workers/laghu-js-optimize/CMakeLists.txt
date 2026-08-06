# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_program(LAGHU_CARGO_EXECUTABLE cargo REQUIRED)
set(
  LAGHU_JS_TARGET_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/cargo"
  CACHE PATH "Cargo target directory for the JavaScript optimizer"
)
set(LAGHU_JS_CARGO_TARGET_ARGS)
if(WIN32 AND CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(LAGHU_JS_RUST_TARGET "aarch64-pc-windows-msvc")
  list(APPEND LAGHU_JS_CARGO_TARGET_ARGS --target "${LAGHU_JS_RUST_TARGET}")
  set(LAGHU_JS_BINARY "${LAGHU_JS_TARGET_DIR}/${LAGHU_JS_RUST_TARGET}/release/laghu-js-optimize.exe")
else()
  set(LAGHU_JS_BINARY "${LAGHU_JS_TARGET_DIR}/release/laghu-js-optimize${CMAKE_EXECUTABLE_SUFFIX}")
endif()
set(LAGHU_JS_BINARY "${LAGHU_JS_BINARY}" PARENT_SCOPE)
add_custom_command(
  OUTPUT "${LAGHU_JS_BINARY}"
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${LAGHU_JS_TARGET_DIR}
          ${LAGHU_CARGO_EXECUTABLE} build --locked --release
          ${LAGHU_JS_CARGO_TARGET_ARGS}
          --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
  DEPENDS Cargo.toml Cargo.lock build.rs src/main.rs
          "${CMAKE_SOURCE_DIR}/libs/laghu-runtime/persisted_state_contract.def"
  VERBATIM
)
add_custom_target(laghu-js-optimize ALL DEPENDS "${LAGHU_JS_BINARY}")

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_runtime_persisted_state_conformance_test
    "${CMAKE_SOURCE_DIR}/libs/laghu-runtime/tests/persisted_state_conformance_test.c"
  )
  add_dependencies(laghu_runtime_persisted_state_conformance_test laghu-js-optimize)
  target_link_libraries(laghu_runtime_persisted_state_conformance_test
    PRIVATE Laghu::Runtime laghu_runtime_test_fixture
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_runtime_persisted_state_conformance_test
      PRIVATE -Wall -Wextra -Wpedantic -Werror
    )
  endif()
  add_test(NAME laghu_runtime_persisted_state_conformance_test
    COMMAND laghu_runtime_persisted_state_conformance_test "${LAGHU_JS_BINARY}"
  )
  add_test(NAME laghu_js_persisted_state_conformance_test
    COMMAND ${CMAKE_COMMAND} -E env
      "CARGO_TARGET_DIR=${LAGHU_JS_TARGET_DIR}"
      ${LAGHU_CARGO_EXECUTABLE} test --locked cross_language_conformance
      --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
  )
  set_tests_properties(laghu_js_persisted_state_conformance_test PROPERTIES
    ENVIRONMENT "LAGHU_CROSS_C_HELPER=$<TARGET_FILE:laghu_runtime_persisted_state_conformance_test>"
  )
endif()
