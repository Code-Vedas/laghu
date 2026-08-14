---
title: Packaging and Release Engineering
parent: Developer Guide
nav_order: 7
permalink: /developer-guide/releasing/
---

# Packaging and Release Engineering

The monorepo ships deb, RPM, Homebrew, containers, Helm, matched NGINX/Apache artifacts, the standalone server, and shared worker packages.

## Release Inputs

Pin source archives, hashes, compiler/runtime requirements, NGINX/Apache compatibility, Rust `Cargo.lock`, libvips/codec versions, provider defaults, service definitions, and generated manifests.
Build official SWC workers from the locked graph rather than downloading platform binaries.

## Image Runtime and Codec Updates

`packaging/runtime-dependencies.conf` is the runtime contract for libvips. It defines the minimum
libvips version and mandatory codec capability bits; AVIF is explicitly tested for the bundled native
containers. Distribution package names alone are not capability evidence: release validation runs
`scripts/check-runtime-dependencies`, which reads `laghu-libvips --probe` and rejects a missing image
backend or required codec operation.

When updating libvips, a codec package, or a base image: update the contract only when necessary;
build and smoke NGINX, Apache, and standalone; run image decode/format tests; run the container and
package lanes; and record the resulting probe output in release evidence. If an optional codec is
unavailable, Laghu must keep serving the original or another validated format; never advertise the
missing format. No manual runtime library copy or unverified prebuilt codec is a supported install path.

## Acceptance

Every release runs native architecture matrices, matched-server cold/warm smoke, standalone origin/TLS/lifecycle tests, package install/upgrade/downgrade/uninstall checks, rootless/read-only container tests, persistent-cache rolling upgrades, configuration validation, SBOM/provenance generation, signature verification, and vulnerability scanning.

## Publication

Publish adapter and worker packages as one compatible version set.
Sign repositories and container manifests, publish checksums and provenance, retain previous supported artifacts for rollback, and update compatibility/documentation only from completed evidence.

## Post-Publication

Install from public endpoints into clean environments, verify services and example configuration, run smoke traffic, confirm signatures/SBOMs, and preserve release evidence.
Security fixes follow the published response process and supported release branches.
