# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED CXX)
  message(FATAL_ERROR "Laghu build identity determinism expectation requires LAGHU_SOURCE and CXX")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/build-identity-determinism")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
file(GLOB source_entries RELATIVE "${LAGHU_SOURCE}" "${LAGHU_SOURCE}/*")
list(REMOVE_ITEM source_entries build)
file(RELATIVE_PATH active_binary_relative "${LAGHU_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}")
if(NOT active_binary_relative MATCHES "^\\.\\." AND NOT active_binary_relative STREQUAL ".")
  string(REGEX MATCH "^[^/]+" active_binary_root "${active_binary_relative}")
  if(NOT active_binary_root STREQUAL "")
    list(REMOVE_ITEM source_entries "${active_binary_root}")
  endif()
endif()
foreach(copy_name IN ITEMS source-a source-b source-changed source-benchmark-changed
                          source-adapter-lifecycle-changed source-password-auth-a
                          source-password-auth-private-header-changed)
  file(MAKE_DIRECTORY "${work}/${copy_name}")
  foreach(entry IN LISTS source_entries)
    file(COPY "${LAGHU_SOURCE}/${entry}" DESTINATION "${work}/${copy_name}")
  endforeach()
endforeach()
file(APPEND "${work}/source-changed/VERSION" "\n")
file(APPEND "${work}/source-benchmark-changed/bench/runner.cpp" "\n")
file(APPEND "${work}/source-adapter-lifecycle-changed/src/adapters/dependency_lifecycle.cpp" "\n")
file(APPEND "${work}/source-password-auth-private-header-changed/src/adapters/private/laghu/adapters/internal/password_auth.hpp" "\n")

function(laghu_configure_identity source binary)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${binary}" -G Ninja
      "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_CXX_FLAGS=${CXXFLAGS}"
      ${ARGN}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
  if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Laghu build identity determinism expectation failed: configure_failed\n${configure_output}${configure_error}")
  endif()
endfunction()

laghu_configure_identity("${work}/source-a" "${work}/build-a")
laghu_configure_identity("${work}/source-b" "${work}/build-b")
laghu_configure_identity("${work}/source-changed" "${work}/build-changed")
laghu_configure_identity("${work}/source-benchmark-changed" "${work}/build-benchmark-changed")
laghu_configure_identity("${work}/source-adapter-lifecycle-changed" "${work}/build-adapter-lifecycle-changed")
laghu_configure_identity("${work}/source-password-auth-a" "${work}/build-password-auth-a"
  "-DLAGHU_BUILD_PROFILE=CUSTOM" "-DLAGHU_FEATURES=password_auth")
laghu_configure_identity("${work}/source-password-auth-private-header-changed"
  "${work}/build-password-auth-private-header-changed"
  "-DLAGHU_BUILD_PROFILE=CUSTOM" "-DLAGHU_FEATURES=password_auth")
file(READ "${work}/build-a/config/laghu-build-manifest-v1.json" manifest_a)
file(READ "${work}/build-b/config/laghu-build-manifest-v1.json" manifest_b)
file(READ "${work}/build-changed/config/laghu-build-manifest-v1.json" manifest_changed)
file(READ "${work}/build-benchmark-changed/config/laghu-build-manifest-v1.json" manifest_benchmark_changed)
file(READ "${work}/build-adapter-lifecycle-changed/config/laghu-build-manifest-v1.json" manifest_adapter_lifecycle_changed)
file(READ "${work}/build-password-auth-a/config/laghu-build-manifest-v1.json" manifest_password_auth_a)
file(READ "${work}/build-password-auth-private-header-changed/config/laghu-build-manifest-v1.json" manifest_password_auth_private_header_changed)
if(NOT manifest_a STREQUAL manifest_b)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: equivalent_manifests_differ")
endif()
string(JSON build_id_a GET "${manifest_a}" build_id)
string(JSON build_id_changed GET "${manifest_changed}" build_id)
string(JSON build_id_benchmark_changed GET "${manifest_benchmark_changed}" build_id)
string(JSON build_id_adapter_lifecycle_changed GET "${manifest_adapter_lifecycle_changed}" build_id)
string(JSON build_id_password_auth_a GET "${manifest_password_auth_a}" build_id)
string(JSON build_id_password_auth_private_header_changed GET "${manifest_password_auth_private_header_changed}" build_id)
if(build_id_a STREQUAL build_id_changed)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: material_input_did_not_change_build_id")
endif()
if(build_id_a STREQUAL build_id_benchmark_changed)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: benchmark_input_did_not_change_build_id")
endif()
if(build_id_a STREQUAL build_id_adapter_lifecycle_changed)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: adapter_lifecycle_input_did_not_change_build_id")
endif()
if(build_id_password_auth_a STREQUAL build_id_password_auth_private_header_changed)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: password_auth_private_header_did_not_change_build_id")
endif()
foreach(manifest IN ITEMS "${manifest_a}" "${manifest_changed}" "${manifest_benchmark_changed}"
                      "${manifest_adapter_lifecycle_changed}" "${manifest_password_auth_a}"
                      "${manifest_password_auth_private_header_changed}")
  if(manifest MATCHES "${LAGHU_SOURCE}" OR manifest MATCHES "${work}")
    message(FATAL_ERROR "Laghu build identity determinism expectation failed: filesystem_path_leak")
  endif()
endforeach()
