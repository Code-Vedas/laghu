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

These are required CI lanes, not architecture-independent claims. A release is
eligible only after every applicable native runner and package-install check
succeeds.

| Target | Coverage | Status meaning |
| --- | --- | --- |
| NGINX stable 1.30.4 | Linux x86_64/arm64 and macOS Intel/Apple Silicon build and smoke | Current stable module compatibility |
| NGINX mainline 1.31.3 | Linux x86_64/arm64 build and smoke | Early compatibility signal |
| Apache HTTP Server 2.4 | Linux x86_64/arm64 and macOS Intel/Apple Silicon build and smoke | Native `mod_laghu` adapter compatibility |
| Core/image policy tests | Linux x86_64/arm64, macOS Intel/Apple Silicon, Windows x86_64/arm64 | Server-independent policy and parser behavior |
| Queue/cache and libvips service | Linux, macOS, and Windows x86_64/arm64 | Queue, crash/restart, deadline, and atomic publication behavior |
| libvips enabled | Ubuntu 24.04 x86_64/arm64 and current Intel/Apple Silicon macOS packages | JPEG, PNG, static/animated GIF, and WebP fixture transforms |
| libvips disabled | Linux x86_64 tests | Capability degradation and original preservation |
| Debian packages | Native Ubuntu x86_64/arm64 install, `nginx -t`, and `apache2ctl configtest` | Both ABI-bound offerings |
| RPM packages | Native Fedora x86_64/arm64 install, `nginx -t`, and `httpd -t` | Both ABI-bound offerings |
| Containers | Native Linux x86_64/arm64 smoke | Separate matched `ngx-laghu` and `mod-laghu` images |

The module uses NGINX's `--with-compat` dynamic-module path. A compiled module
must still match the target installation's NGINX version, architecture, and
compatible build signature.

Windows uses native file mappings, immediate-fail file locks, process Job
Objects, atomic file replacement, and a Windows service. Windows server
installers contain matched NGINX or Apache builds rather than claiming that one
module binary fits unrelated server builds.

## Compatibility Scope

The full scope includes Apache 2.4, current NGINX stable and mainline releases,
OpenResty, Angie, freenginx, distribution packages, containers, Helm, HTTP/2,
HTTP/3, and sidecar or reverse-proxy operation.

The validated matrix grows as repeatable build, runtime, protocol, and package
tests are added. Evidence status in the matrix describes validation depth only;
it does not redefine or narrow Laghu's compatibility scope.
