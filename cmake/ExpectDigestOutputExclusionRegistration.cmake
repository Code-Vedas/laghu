# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED TEST_FILE OR NOT DEFINED CROSSCOMPILING)
  message(FATAL_ERROR "Laghu digest output exclusion registration test requires test file and cross state")
endif()
if(NOT EXISTS "${TEST_FILE}")
  message(FATAL_ERROR "Laghu digest output exclusion registration test is missing CTestTestfile")
endif()

file(READ "${TEST_FILE}" registered_tests)
if(NOT registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\\"" AND
    NOT registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\]=\\]")
  message(FATAL_ERROR "Laghu digest output exclusion registration test is missing static coverage")
endif()
if(CROSSCOMPILING)
  if(registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\.version\\\"" OR
      registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\.version\\]=\\]")
    message(FATAL_ERROR "Laghu digest output exclusion registration test registers a target binary in cross mode")
  endif()
else()
  if(NOT registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\.version\\\"" AND
      NOT registered_tests MATCHES "laghu\\.core\\.digest_primitives\\.output_exclusion\\.version\\]=\\]")
    message(FATAL_ERROR "Laghu digest output exclusion registration test is missing native version coverage")
  endif()
endif()
