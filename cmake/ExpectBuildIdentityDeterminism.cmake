# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED LAGHU_SOURCE OR NOT DEFINED CXX)
  message(FATAL_ERROR "Laghu build identity determinism expectation requires LAGHU_SOURCE and CXX")
endif()

set(work "${CMAKE_CURRENT_BINARY_DIR}/build-identity-determinism")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
file(GLOB source_entries RELATIVE "${LAGHU_SOURCE}" "${LAGHU_SOURCE}/*")
foreach(copy_name IN ITEMS source-a source-b source-changed)
  file(MAKE_DIRECTORY "${work}/${copy_name}")
  foreach(entry IN LISTS source_entries)
    file(COPY "${LAGHU_SOURCE}/${entry}" DESTINATION "${work}/${copy_name}")
  endforeach()
endforeach()
file(APPEND "${work}/source-changed/VERSION" "\n")

function(laghu_configure_identity source binary)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${binary}" -G Ninja
      "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_CXX_FLAGS=${CXXFLAGS}"
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
file(READ "${work}/build-a/config/laghu-build-manifest-v1.json" manifest_a)
file(READ "${work}/build-b/config/laghu-build-manifest-v1.json" manifest_b)
file(READ "${work}/build-changed/config/laghu-build-manifest-v1.json" manifest_changed)
if(NOT manifest_a STREQUAL manifest_b)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: equivalent_manifests_differ")
endif()
string(JSON build_id_a GET "${manifest_a}" build_id)
string(JSON build_id_changed GET "${manifest_changed}" build_id)
if(build_id_a STREQUAL build_id_changed)
  message(FATAL_ERROR "Laghu build identity determinism expectation failed: material_input_did_not_change_build_id")
endif()
foreach(manifest IN ITEMS "${manifest_a}" "${manifest_changed}")
  if(manifest MATCHES "${LAGHU_SOURCE}" OR manifest MATCHES "${work}")
    message(FATAL_ERROR "Laghu build identity determinism expectation failed: filesystem_path_leak")
  endif()
endforeach()
