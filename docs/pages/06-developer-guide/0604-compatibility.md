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
| deb/RPM/Homebrew | Package build, installation, configuration validation, upgrade policy, lifecycle, and removal checks. |
| Containers | Target-architecture build plus runtime smoke and persistent-directory checks. |

The NGINX `--with-compat` path does not make one module universal across versions, architectures, or build signatures.
Apache modules remain bound to their server/APR environment.
Cross-architecture Buildx/QEMU runs provide developer feedback but do not replace native release evidence.

Before publishing a release, verify the current matrices in CI and packaging rather than copying an older version list into documentation.
Every advertised package, architecture, server, container, and lifecycle must retain current automated evidence.

## NGINX Support Policy

Laghu supports the current upstream stable and mainline NGINX source releases recorded in
[`packaging/nginx/release-policy.conf`](https://github.com/Code-Vedas/laghu/blob/main/packaging/nginx/release-policy.conf).
They are matched-build support claims: each release must pass module build, configuration validation,
and cold/warm runtime smoke before it is published. Older releases are supported only where a release
artifact and its completed compatibility evidence are retained.

`scripts/check-nginx-release-policy` compares the two pins with the upstream NGINX release page.
CI runs it on every change and weekly, then builds and smokes both discovered current releases on a
native Linux runner. A changed upstream release fails the policy check until the pin is deliberately
updated and the required evidence succeeds; this is not an automatic upgrade.
