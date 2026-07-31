---
title: Compatibility and Releases
parent: Developer Guide
nav_order: 4
permalink: /developer-guide/compatibility/
---

# Compatibility and Releases

Compatibility is an evidence claim, not a portability inference.

| Surface | Required evidence |
| --- | --- |
| Shared libraries and workers | Native build and tests for the claimed OS/architecture. |
| NGINX module | Matched server build, configuration test, and cold/warm runtime smoke. |
| Apache module | Matched Apache/APR build, configuration test, and cold/warm runtime smoke. |
| Standalone server | Native proxy tests, origin/TLS behavior, cold/warm smoke, and lifecycle validation. |
| deb/RPM/Homebrew/Windows | Package build, installation, configuration validation, upgrade policy, lifecycle, and removal checks. |
| Containers | Target-architecture build plus runtime smoke and persistent-directory checks. |

The NGINX `--with-compat` path does not make one module universal across versions, architectures, or build signatures.
Apache modules remain bound to their server/APR environment.
Cross-architecture Buildx/QEMU runs provide developer feedback but do not replace native release evidence.

Before publishing a release, verify the current matrices in CI and packaging rather than copying an older version list into documentation.
Unvalidated roadmap targets must remain explicitly pending.
