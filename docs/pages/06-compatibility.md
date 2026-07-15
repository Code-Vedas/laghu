---
title: Compatibility
nav_order: 6
permalink: /compatibility/
---

# Compatibility

Laghu's compatibility scope includes every server, protocol, packaging, and
deployment target listed below. The matrix records targets with automated
evidence today; it is not an allowlist and does not exclude any target.

## Automated Build Matrix

| Target | Coverage | Status meaning |
| --- | --- | --- |
| NGINX stable 1.30.3 | Ubuntu dynamic-module compile | Build compatibility |
| NGINX mainline 1.31.2 | Ubuntu dynamic-module compile | Early compatibility signal |
| `laghu-core` | Ubuntu and macOS CMake/CTest | Server-independent policy behavior |

The module uses NGINX's `--with-compat` dynamic-module path. A compiled module
must still match the target installation's NGINX version, architecture, and
compatible build signature.

## Compatibility Scope

The full scope includes current NGINX stable and mainline releases, OpenResty,
Angie, freenginx, dynamic modules, distribution packages, container images,
Helm deployments, HTTP/2 and HTTP/3 delivery, and sidecar or reverse-proxy
operation.

The validated matrix grows as repeatable build, runtime, protocol, and package
tests are added. Evidence status in the matrix describes validation depth only;
it does not redefine or narrow Laghu's compatibility scope.
