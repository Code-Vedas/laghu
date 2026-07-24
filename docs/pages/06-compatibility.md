---
title: Compatibility
nav_order: 6
permalink: /compatibility/
---

# Compatibility

The matrix records targets with automated build and runtime evidence.

## Automated Build Matrix

These are required CI lanes, not architecture-independent claims. A release is eligible only after every applicable native runner and package-install check succeeds.

| Target | Coverage | Status meaning |
| --- | --- | --- |
| NGINX stable 1.30.4 | Linux Docker amd64/arm64 and macOS Apple Silicon build and smoke | Stable module compatibility |
| NGINX mainline 1.31.3 | Linux Docker amd64/arm64 build and smoke | Mainline module compatibility |
| Apache HTTP Server 2.4 | Linux Docker amd64/arm64 and macOS Apple Silicon build and smoke | Native `mod_laghu` compatibility |
| Core/image policy tests | Linux Docker amd64/arm64 and macOS Apple Silicon | Server-independent policy and parser behavior |
| Queue/cache and libvips service | Linux Docker amd64/arm64 and macOS Apple Silicon | Queue, crash/restart, deadline, and atomic publication behavior |
| libvips enabled | Ubuntu 24.04 Docker amd64/arm64 and macOS Apple Silicon | JPEG, PNG, static/animated GIF, and WebP fixture transforms |
| libvips disabled | Linux x86_64 tests | Capability degradation and original preservation |
| Debian packages | Debian/Ubuntu Docker amd64/arm64 package build, install, and server configuration validation | ABI-bound package behavior |
| RPM packages | Fedora/Rocky/Alma Docker amd64/arm64 package build, install, and server configuration validation | ABI-bound package behavior |

The module uses NGINX's `--with-compat` dynamic-module path. A compiled module must still match the target installation's NGINX version, architecture, and compatible build signature.
