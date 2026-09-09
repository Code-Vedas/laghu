# SPDX-License-Identifier: AGPL-3.0-only

if(NOT DEFINED SOURCE OR NOT DEFINED ARTIFACT OR NOT DEFINED NM)
  message(FATAL_ERROR "Laghu core digest boundary test requires source, artifact, and nm")
endif()

set(public_implementation "${SOURCE}/src/core/digest.cpp")
set(secret_implementation "${SOURCE}/src/core/fingerprints.cpp")
set(public_contract "${SOURCE}/src/core/contract/laghu/core/digest.hpp")
set(secret_contract "${SOURCE}/src/core/private/laghu/core/internal/fingerprints.hpp")
if(NOT EXISTS "${public_implementation}" OR NOT EXISTS "${secret_implementation}" OR
    NOT EXISTS "${public_contract}" OR NOT EXISTS "${secret_contract}" OR NOT EXISTS "${ARTIFACT}")
  message(FATAL_ERROR "Laghu core digest boundary test is missing a required artifact")
endif()

file(READ "${public_implementation}" public_implementation_text)
file(READ "${secret_implementation}" secret_implementation_text)
file(READ "${public_contract}" public_contract_text)
file(READ "${secret_contract}" secret_contract_text)
if(public_implementation_text MATCHES "laghu/adapters" OR
    secret_implementation_text MATCHES "laghu/adapters" OR
    public_contract_text MATCHES "laghu/adapters" OR secret_contract_text MATCHES "laghu/adapters")
  message(FATAL_ERROR "Laghu core digest boundary test found an adapter dependency in core")
endif()
if(public_contract_text MATCHES "SecretFingerprint|FingerprintKeyId" OR
    secret_contract_text MATCHES "serialize|encode_[a-z_]*\\(")
  message(FATAL_ERROR "Laghu core digest boundary test found a fingerprint serialization API")
endif()

execute_process(
  COMMAND "${NM}" -u "${ARTIFACT}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "Laghu core digest boundary test failed to inspect archive: ${nm_error}")
endif()
if(nm_output MATCHES "adapters")
  message(FATAL_ERROR "Laghu core digest boundary test found an adapter symbol in core")
endif()
