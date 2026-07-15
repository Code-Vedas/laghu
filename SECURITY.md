# Security Policy

## Supported Versions

Laghu is pre-release. Until the first supported release, security fixes target
the default branch. Supported release lines will be listed here and in release
notes.

## Reporting a Vulnerability

Use GitHub's private vulnerability reporting for this repository. If that is
unavailable, contact <admin@codevedas.com>. Include the affected revision,
impact, reproduction details that are safe to share, and any known mitigation.

Do not open a public issue for an unpatched vulnerability.

## Security Boundaries

Laghu runs inside NGINX worker processes and can inspect response content. Code
in the module therefore receives the same trust as the server process. Changes
that add resource fetching, parsing, image codecs, shared storage, or worker IPC
must include explicit threat modeling and fail-open coverage.

Private and loopback fetches remain disabled by default. Authenticated,
`private`, and `no-store` responses must bypass optimization unless a future
documented configuration explicitly opts into a safe behavior.

