# SPDX-License-Identifier: AGPL-3.0-only
include_guard(GLOBAL)

function(laghu_add_native_test name)
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()
  add_test(NAME "${name}" COMMAND ${ARGN})
endfunction()
