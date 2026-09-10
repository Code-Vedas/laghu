# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

# This module is the sole declaration of Laghu's internal subsystem graph.
# Protocol and OS platform leaves are implementation directories, not graph
# nodes: http1/http2/http3 belong to protocol and linux/freebsd/macos to os.
set(LAGHU_SUBSYSTEM_NODES
  core config os protocol tls cache observability proxy control cli adapters)

function(laghu_dag_set_allowed consumer)
  set_property(GLOBAL PROPERTY "LAGHU_DAG_ALLOWED_${consumer}" "${ARGN}")
endfunction()

laghu_dag_set_allowed(core)
laghu_dag_set_allowed(config core)
laghu_dag_set_allowed(os core)
laghu_dag_set_allowed(protocol core config os)
laghu_dag_set_allowed(tls core config os)
laghu_dag_set_allowed(cache core config os)
laghu_dag_set_allowed(observability core config os)
laghu_dag_set_allowed(proxy core config os protocol tls cache observability)
laghu_dag_set_allowed(control core config os proxy cache observability)
laghu_dag_set_allowed(cli core config control)
laghu_dag_set_allowed(adapters core config os protocol tls cache observability proxy control cli)

function(laghu_dag_require_node node role)
  list(FIND LAGHU_SUBSYSTEM_NODES "${node}" node_index)
  if(node_index EQUAL -1)
    if(node MATCHES "^(common|shared|utility|utils)$")
      message(FATAL_ERROR
        "Laghu dependency DAG failed: ${role}=${node} rule=no_generic_utility_sink")
    endif()
    message(FATAL_ERROR
      "Laghu dependency DAG failed: ${role}=${node} rule=unknown_subsystem")
  endif()
endfunction()

function(laghu_register_subsystem_target target node)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR
      "Laghu dependency DAG failed: target=${target} rule=target_must_exist")
  endif()
  laghu_dag_require_node("${node}" "subsystem")
  get_property(existing_node TARGET "${target}" PROPERTY LAGHU_SUBSYSTEM_NODE)
  if(existing_node AND NOT existing_node STREQUAL "${node}")
    message(FATAL_ERROR
      "Laghu dependency DAG failed: target=${target} rule=target_registered_twice")
  endif()
  set_property(TARGET "${target}" PROPERTY LAGHU_SUBSYSTEM_NODE "${node}")
endfunction()

function(laghu_dag_node_for_target target output)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR
      "Laghu dependency DAG failed: target=${target} rule=target_must_exist")
  endif()
  get_property(node TARGET "${target}" PROPERTY LAGHU_SUBSYSTEM_NODE)
  if(NOT node)
    message(FATAL_ERROR
      "Laghu dependency DAG failed: target=${target} rule=target_not_a_subsystem")
  endif()
  set(${output} "${node}" PARENT_SCOPE)
endfunction()

function(laghu_dag_reaches start goal output)
  if(start STREQUAL goal)
    set(${output} TRUE PARENT_SCOPE)
    return()
  endif()
  get_property(dependencies GLOBAL PROPERTY "LAGHU_DAG_DEPENDS_${start}")
  foreach(dependency IN LISTS dependencies)
    laghu_dag_reaches("${dependency}" "${goal}" reaches_goal)
    if(reaches_goal)
      set(${output} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${output} FALSE PARENT_SCOPE)
endfunction()

function(laghu_link_subsystems consumer)
  laghu_dag_node_for_target("${consumer}" consumer_node)
  get_target_property(consumer_type "${consumer}" TYPE)
  if(consumer_type STREQUAL "INTERFACE_LIBRARY")
    set(link_scope INTERFACE)
  else()
    set(link_scope PRIVATE)
  endif()

  foreach(provider IN LISTS ARGN)
    laghu_dag_node_for_target("${provider}" provider_node)

    # Test this first so an attempted back-edge reports the structural cause
    # rather than merely the narrower edge policy violation.
    laghu_dag_reaches("${provider_node}" "${consumer_node}" creates_cycle)
    if(creates_cycle)
      message(FATAL_ERROR
        "Laghu dependency DAG failed: consumer=${consumer_node} provider=${provider_node} rule=cycle_forbidden")
    endif()
    if(provider_node STREQUAL "adapters")
      message(FATAL_ERROR
        "Laghu dependency DAG failed: consumer=${consumer_node} provider=${provider_node} rule=adapter_direction_inward_only")
    endif()
    get_property(allowed_providers GLOBAL PROPERTY "LAGHU_DAG_ALLOWED_${consumer_node}")
    list(FIND allowed_providers "${provider_node}" allowed_index)
    if(allowed_index EQUAL -1)
      if(consumer_node STREQUAL "core")
        set(rule core_has_no_outward_dependencies)
      elseif(consumer_node STREQUAL "os")
        set(rule os_may_depend_only_on_core)
      elseif(consumer_node STREQUAL "adapters" AND provider_node STREQUAL "adapters")
        set(rule adapter_may_not_depend_on_itself)
      else()
        set(rule edge_not_allowed)
      endif()
      message(FATAL_ERROR
        "Laghu dependency DAG failed: consumer=${consumer_node} provider=${provider_node} rule=${rule}")
    endif()

    target_link_libraries("${consumer}" ${link_scope} "${provider}")
    get_property(existing_dependencies GLOBAL PROPERTY "LAGHU_DAG_DEPENDS_${consumer_node}")
    list(APPEND existing_dependencies "${provider_node}")
    list(REMOVE_DUPLICATES existing_dependencies)
    set_property(GLOBAL PROPERTY "LAGHU_DAG_DEPENDS_${consumer_node}" "${existing_dependencies}")
  endforeach()
endfunction()

function(laghu_render_dependency_dag output)
  set(rendered "")
  foreach(consumer IN LISTS LAGHU_SUBSYSTEM_NODES)
    get_property(providers GLOBAL PROPERTY "LAGHU_DAG_ALLOWED_${consumer}")
    if(providers)
      string(JOIN ", " provider_text ${providers})
    else()
      set(provider_text "none")
    endif()
    string(APPEND rendered "${consumer} -> ${provider_text}\n")
  endforeach()
  set(${output} "${rendered}" PARENT_SCOPE)
endfunction()

function(laghu_validate_dependency_dag_documentation path)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Laghu dependency DAG failed: documentation=${path} rule=missing")
  endif()
  laghu_render_dependency_dag(expected_graph)
  file(READ "${path}" documentation)
  string(FIND "${documentation}" "${expected_graph}" graph_offset)
  if(graph_offset EQUAL -1)
    message(FATAL_ERROR
      "Laghu dependency DAG failed: documentation=${path} rule=checked_graph_does_not_match_declarations")
  endif()
endfunction()

function(laghu_declare_subsystem_graph)
  laghu_register_subsystem_target(laghu_core core)
  foreach(node IN ITEMS config os protocol tls cache observability proxy control cli adapters)
    if(node STREQUAL "os")
      add_library(laghu_os STATIC src/os/io_slices.cpp src/os/io_operations.cpp)
    else()
      add_library("laghu_${node}" INTERFACE)
    endif()
    set_property(TARGET "laghu_${node}" PROPERTY CXX_VISIBILITY_PRESET hidden)
    set_property(TARGET "laghu_${node}" PROPERTY VISIBILITY_INLINES_HIDDEN YES)
    laghu_register_subsystem_target("laghu_${node}" "${node}")
  endforeach()

  # Materialize target_link_libraries calls from the declarations above, so
  # the allowed-edge table, CMake targets, and checked documentation cannot
  # drift into competing representations of the graph.
  foreach(consumer IN LISTS LAGHU_SUBSYSTEM_NODES)
    get_property(provider_nodes GLOBAL PROPERTY "LAGHU_DAG_ALLOWED_${consumer}")
    foreach(provider_node IN LISTS provider_nodes)
      list(APPEND provider_targets "laghu_${provider_node}")
    endforeach()
    if(provider_targets)
      laghu_link_subsystems("laghu_${consumer}" ${provider_targets})
    endif()
    unset(provider_targets)
  endforeach()

  laghu_validate_dependency_dag_documentation(
    "${CMAKE_SOURCE_DIR}/docs/normative/internal-dependency-dag.md")
endfunction()
