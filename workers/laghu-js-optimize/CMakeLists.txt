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
set(LAGHU_JS_BINARY "${LAGHU_JS_TARGET_DIR}/release/laghu-js-optimize")
set(LAGHU_JS_BINARY "${LAGHU_JS_BINARY}" PARENT_SCOPE)
add_custom_command(
  OUTPUT "${LAGHU_JS_BINARY}"
  COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${LAGHU_JS_TARGET_DIR}
          ${LAGHU_CARGO_EXECUTABLE} build --locked --release
          --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
  DEPENDS Cargo.toml Cargo.lock build.rs src/main.rs src/queue.rs src/operational.rs src/publication.rs src/wire.rs
          "${CMAKE_SOURCE_DIR}/libs/laghu-runtime/persisted_state_contract.def"
  VERBATIM
)
add_custom_target(laghu-js-optimize ALL DEPENDS "${LAGHU_JS_BINARY}")

if(LAGHU_BUILD_TESTS)
  laghu_add_runtime_test(
    persisted_state_conformance
    FIXTURE
    COMMAND_ARGS "${LAGHU_JS_BINARY}"
  )
  add_dependencies(laghu_runtime_persisted_state_conformance_test laghu-js-optimize)
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
