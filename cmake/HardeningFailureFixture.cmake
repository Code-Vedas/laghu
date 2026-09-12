# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE OR NOT DEFINED FEATURE)
  message(FATAL_ERROR "Laghu hardening failure fixture requires MODULE and FEATURE")
endif()

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_SYSTEM_NAME Linux)
set(UNIX TRUE)
include("${MODULE}")

function(laghu_hardening_probe feature output)
  if("${feature}" STREQUAL "${FEATURE}")
    set(${output} false PARENT_SCOPE)
  else()
    set(${output} true PARENT_SCOPE)
  endif()
endfunction()

laghu_configure_hardening()
