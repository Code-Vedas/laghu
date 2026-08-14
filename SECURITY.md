# Security Policy

## Supported Versions

Laghu is pre-release. Security fixes target the default branch until a supported release branch is
announced. A release branch is supported only while its release notes and security advisory state that
it receives fixes; unsupported branches receive no backports.

## Reporting a Vulnerability

Use GitHub's private vulnerability reporting for this repository. If that is unavailable, contact <admin@codevedas.com>. Include the affected revision, impact, reproduction details that are safe to share, and any known mitigation.

Do not open a public issue for an unpatched vulnerability.

## Response Process

We acknowledge a private report within five business days, triage severity and affected surfaces within
ten business days, and provide a private status update at least every ten business days until resolution.
Timelines may change when coordinated disclosure, an upstream fix, or a reporter embargo requires it;
we will communicate that privately.

Maintain fixes in a private advisory while they are embargoed. Before disclosure, validate the patch on
every affected delivery surface, publish a versioned release, and prepare upgrade and mitigation notes.
After disclosure, publish the GitHub Security Advisory with CVE assignment when eligible, affected and
fixed versions, severity, acknowledgements when permitted, and operator mitigation. Do not claim a CVE
or a fixed version before the release artifact exists.

Security releases must run the release workflow, include SBOM/provenance and signature artifacts, and
preserve the completed compatibility and package-install evidence. If a bundled dependency is affected,
the advisory identifies the dependency, supported package/container versions, and whether fail-open
behavior limits exposure; it does not substitute that behavior for a patch.

## Security Boundaries

Laghu's native modules run inside NGINX or Apache processes and can inspect response content. Adapter code therefore receives the same trust as its server process. Changes that add resource fetching, parsing, image codecs, shared storage, or worker IPC must include explicit threat modeling and fail-open coverage.

Private and loopback fetches are disabled. Authenticated, `private`, and `no-store` responses bypass optimization.
