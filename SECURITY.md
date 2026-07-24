# Security Policy

## Supported Versions

Laghu is pre-release. Security fixes target the default branch.

## Reporting a Vulnerability

Use GitHub's private vulnerability reporting for this repository. If that is unavailable, contact <admin@codevedas.com>. Include the affected revision, impact, reproduction details that are safe to share, and any known mitigation.

Do not open a public issue for an unpatched vulnerability.

## Security Boundaries

Laghu's native modules run inside NGINX or Apache processes and can inspect response content. Adapter code therefore receives the same trust as its server process. Changes that add resource fetching, parsing, image codecs, shared storage, or worker IPC must include explicit threat modeling and fail-open coverage.

Private and loopback fetches are disabled. Authenticated, `private`, and `no-store` responses bypass optimization.
