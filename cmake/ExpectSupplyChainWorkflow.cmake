# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED WORKFLOW OR NOT EXISTS "${WORKFLOW}")
  message(FATAL_ERROR "Laghu supply-chain workflow expectation requires WORKFLOW")
endif()
file(READ "${WORKFLOW}" workflow)
foreach(required IN ITEMS
    "if: github.event_name == 'push' && github.ref == 'refs/heads/main'"
    "actions/attest@1e69f48acb82d1966a394da916b4c1698aa569d6"
    "contents: read"
    "id-token: write"
    "attestations: write")
  string(FIND "${workflow}" "${required}" required_index)
  if(required_index EQUAL -1)
    message(FATAL_ERROR "Laghu supply-chain workflow expectation failed: missing=${required}")
  endif()
endforeach()
foreach(forbidden IN ITEMS "private-key" "signing-key" "id-token: read" "attestations: read")
  string(FIND "${workflow}" "${forbidden}" forbidden_index)
  if(NOT forbidden_index EQUAL -1)
    message(FATAL_ERROR "Laghu supply-chain workflow expectation failed: forbidden=${forbidden}")
  endif()
endforeach()
string(REGEX MATCH
  "attest-supply-chain:[^|]*permissions:[ ]*\n[ ]+contents: read[ ]*\n[ ]+id-token: write[ ]*\n[ ]+attestations: write"
  permissions "${workflow}")
if(permissions STREQUAL "")
  message(FATAL_ERROR "Laghu supply-chain workflow expectation failed: least_privilege_permissions")
endif()
