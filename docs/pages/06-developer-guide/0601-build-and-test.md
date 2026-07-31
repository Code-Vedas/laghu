---
title: Build and Test
parent: Developer Guide
nav_order: 1
permalink: /developer-guide/build-and-test/
---

# Build and Test

## Prerequisites

The core build requires CMake and a C11 compiler.
Image support requires libvips and its codec libraries.
JavaScript optimization requires the Rust toolchain version accepted by the locked worker graph.
Native adapter smoke tests additionally require the matching NGINX or Apache development environment.

## Canonical Validation

```bash
scripts/ci-install-dependencies
scripts/run-all
```

`scripts/run-all` runs license, syntax, ShellCheck, C formatting, CMake/CTest, adapter smoke, and documentation-build lanes when their prerequisites are available.
A skipped prerequisite is not validation evidence.

Useful focused commands include:

```bash
scripts/run-lint-all
scripts/run-build-all
scripts/run-test-all
scripts/run-module-smoke-all
scripts/run-apache-module-smoke-all
scripts/run-proxy-rootless
scripts/run-docs-build-all
git diff --check
```

Do not publish platform support from a cross-compiled object or emulated container alone.
Release evidence requires the applicable native build, runtime, service-lifecycle, and package-install lanes.
