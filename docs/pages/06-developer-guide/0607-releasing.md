---
title: Packaging and Release Engineering
parent: Developer Guide
nav_order: 7
permalink: /developer-guide/releasing/
---

# Packaging and Release Engineering

The monorepo ships deb, RPM, Homebrew, Winget, containers, Helm, matched NGINX/Apache artifacts, the standalone server, and shared worker packages.

## Release Inputs

Pin source archives, hashes, compiler/runtime requirements, NGINX/Apache compatibility, Rust `Cargo.lock`, libvips/codec versions, provider defaults, service definitions, and generated manifests.
Build official SWC workers from the locked graph rather than downloading platform binaries.

## Acceptance

Every release runs native architecture matrices, matched-server cold/warm smoke, standalone origin/TLS/lifecycle tests, package install/upgrade/downgrade/uninstall checks, rootless/read-only container tests, persistent-cache rolling upgrades, configuration validation, SBOM/provenance generation, signature verification, and vulnerability scanning.

## Publication

Publish adapter and worker packages as one compatible version set.
Sign repositories and container manifests, publish checksums and provenance, retain previous supported artifacts for rollback, and update compatibility/documentation only from completed evidence.

## Post-Publication

Install from public endpoints into clean environments, verify services and example configuration, run smoke traffic, confirm signatures/SBOMs, and preserve release evidence.
Security fixes follow the published response process and supported release branches.
