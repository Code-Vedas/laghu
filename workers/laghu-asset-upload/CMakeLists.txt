# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

add_executable(laghu-asset-upload main.c)
target_link_libraries(laghu-asset-upload PRIVATE Laghu::Runtime OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(laghu-asset-upload PRIVATE c_std_11)

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_asset_s3_contract_test s3_contract_test.c)
  target_link_libraries(laghu_asset_s3_contract_test PRIVATE Laghu::Runtime OpenSSL::SSL OpenSSL::Crypto)
  target_compile_features(laghu_asset_s3_contract_test PRIVATE c_std_11)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_asset_s3_contract_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_asset_s3_contract_test COMMAND laghu_asset_s3_contract_test)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu-asset-upload PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
