# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_program(LAGHU_CARGO_EXECUTABLE cargo REQUIRED)
set(LAGHU_JS_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
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
  DEPENDS Cargo.toml Cargo.lock src/main.rs
  VERBATIM
)
add_custom_target(laghu-js-optimize ALL DEPENDS "${LAGHU_JS_BINARY}")
