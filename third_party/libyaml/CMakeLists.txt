# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

add_library(laghu_libyaml STATIC
  src/api.c
  src/dumper.c
  src/emitter.c
  src/loader.c
  src/parser.c
  src/reader.c
  src/scanner.c
  src/writer.c
)
configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h" COPYONLY)
target_include_directories(laghu_libyaml PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(laghu_libyaml PRIVATE HAVE_CONFIG_H YAML_DECLARE_STATIC)
target_compile_features(laghu_libyaml PUBLIC c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_libyaml PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_library(Laghu::Yaml ALIAS laghu_libyaml)

if(BUILD_TESTING)
  add_executable(laghu_libyaml_nesting_test tests/nesting_test.c)
  target_link_libraries(laghu_libyaml_nesting_test PRIVATE Laghu::Yaml)
  target_compile_features(laghu_libyaml_nesting_test PRIVATE c_std_11)
  add_test(NAME laghu_libyaml_nesting_test COMMAND laghu_libyaml_nesting_test)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_libyaml_nesting_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
