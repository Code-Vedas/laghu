# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

function(laghu_configure_otlp_schema output_target output_directory)
  include(ExternalProject)
  include(FetchContent)
  set(host_prefix "${CMAKE_BINARY_DIR}/_host/otlp")
  set(host_install "${host_prefix}/install")
  get_property(protobuf_root GLOBAL PROPERTY
    LAGHU_DEPENDENCY_SOURCE_DIRECTORY_protobuf)
  get_property(protobuf_c_root GLOBAL PROPERTY
    LAGHU_DEPENDENCY_SOURCE_DIRECTORY_protobuf_c)
  if(protobuf_root STREQUAL "" OR protobuf_c_root STREQUAL "")
    message(FATAL_ERROR "Laghu OTLP compiler sources were not acquired")
  endif()
  ExternalProject_Add(laghu_host_protobuf
    SOURCE_DIR "${protobuf_root}"
    SOURCE_SUBDIR cmake PREFIX "${host_prefix}/protobuf"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${host_install}
      -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF
      -Dprotobuf_BUILD_PROTOC_BINARIES=ON -DCMAKE_BUILD_TYPE=Release
    BUILD_BYPRODUCTS "${host_install}/bin/protoc")
  ExternalProject_Add(laghu_host_protobuf_c
    SOURCE_DIR "${protobuf_c_root}"
    SOURCE_SUBDIR build-cmake PREFIX "${host_prefix}/protobuf-c"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${host_install}
      -DCMAKE_PREFIX_PATH=${host_install} -DBUILD_TESTS=OFF -DBUILD_PROTOC=ON
      -DCMAKE_BUILD_TYPE=Release
    DEPENDS laghu_host_protobuf
    BUILD_BYPRODUCTS "${host_install}/bin/protoc-gen-c")
  get_property(proto_root GLOBAL PROPERTY
    LAGHU_DEPENDENCY_SOURCE_DIRECTORY_opentelemetry_proto)
  if(proto_root STREQUAL "")
    message(FATAL_ERROR "Laghu OTLP schema source was not acquired")
  endif()
  set(generated "${CMAKE_BINARY_DIR}/generated/otlp")
  set(schema_input "${CMAKE_BINARY_DIR}/generated/otlp-schema")
  set(protos
    opentelemetry/proto/common/v1/common.proto
    opentelemetry/proto/resource/v1/resource.proto
    opentelemetry/proto/trace/v1/trace.proto
    opentelemetry/proto/metrics/v1/metrics.proto
    opentelemetry/proto/logs/v1/logs.proto
    opentelemetry/proto/collector/trace/v1/trace_service.proto
    opentelemetry/proto/collector/metrics/v1/metrics_service.proto
    opentelemetry/proto/collector/logs/v1/logs_service.proto)
  set(generated_sources)
  set(generated_headers)
  set(proto_inputs)
  foreach(proto IN LISTS protos)
    get_filename_component(proto_directory "${proto}" DIRECTORY)
    file(MAKE_DIRECTORY "${schema_input}/${proto_directory}")
    file(READ "${proto_root}/${proto}" proto_contents)
    # protobuf-c 1.5.2 does not advertise proto3 optional support. Removing
    # the source-level presence qualifier preserves the scalar wire format used
    # by these private generated definitions.
    string(REPLACE "optional " "" proto_contents "${proto_contents}")
    file(WRITE "${schema_input}/${proto}" "${proto_contents}")
    string(REPLACE ".proto" ".pb-c.c" generated_source "${proto}")
    string(REPLACE ".proto" ".pb-c.h" generated_header "${proto}")
    list(APPEND generated_sources "${generated}/${generated_source}")
    list(APPEND generated_headers "${generated}/${generated_header}")
    list(APPEND proto_inputs "${schema_input}/${proto}")
  endforeach()
  add_custom_command(OUTPUT ${generated_sources} ${generated_headers}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated}"
    COMMAND "${host_install}/bin/protoc"
      "--plugin=protoc-gen-c=${host_install}/bin/protoc-gen-c"
      "--proto_path=${schema_input}" "--c_out=${generated}" ${protos}
    DEPENDS laghu_host_protobuf_c ${proto_inputs}
    WORKING_DIRECTORY "${schema_input}" VERBATIM)
  add_library(laghu_otlp_schema STATIC ${generated_sources})
  target_include_directories(laghu_otlp_schema SYSTEM PUBLIC "${generated}")
  target_link_libraries(laghu_otlp_schema PRIVATE laghu_feature_otlp)
  set(${output_target} laghu_otlp_schema PARENT_SCOPE)
  set(${output_directory} "${generated}" PARENT_SCOPE)
endfunction()
