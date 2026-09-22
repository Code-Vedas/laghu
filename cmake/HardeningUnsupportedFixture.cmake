# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED MODULE)
  message(FATAL_ERROR "Laghu hardening unsupported fixture requires MODULE")
endif()

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_SYSTEM_NAME Linux)
set(UNIX TRUE)
include("${MODULE}")

function(laghu_hardening_probe feature output)
  if("${feature}" STREQUAL "fortification")
    set(${output} false PARENT_SCOPE)
  else()
    set(${output} true PARENT_SCOPE)
  endif()
endfunction()

laghu_configure_hardening()
if(NOT LAGHU_HARDENING_METADATA_JSON MATCHES "\"fortification\":\{\"supported\":false,\"enabled\":false,\"state\":\"unsupported\"\}")
  message(FATAL_ERROR "Laghu hardening unsupported fixture failed: fortification=not_recorded")
endif()
