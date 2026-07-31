---
title: Changing a Filter or Setting
parent: Developer Guide
nav_order: 5
permalink: /developer-guide/changing-laghu/
---

# Changing a Filter or Setting

## Start at the Contract

Define eligibility, input/output bounds, ownership, cache dependencies, failure behavior, CSP/security impact, and the exact acceptance gate before editing an adapter.
Put shared policy in `laghu-core`, shared HTTP orchestration in `laghu-http`, planners/catalogs in `laghu-runtime`, image discovery in `laghu-image`, and expensive execution in the appropriate worker.

## Keep All Surfaces Equivalent

A public optimization or setting must reach NGINX, Apache, and standalone parsing, inheritance/default resolution, policy keys, diagnostics, examples, packages, and smoke tests.
Surface-specific syntax may differ, but resolved behavior and fail-open outcomes must remain equivalent.

## Version State Deliberately

Change queue, catalog, parser/planner, cache-key, provider, SWC/profile, RUM, or decision versions whenever an old record could be misinterpreted or served under broader policy.
Reject incompatible state instead of guessing compatibility.

## Testing Ladder

1. Add bounded parser/policy unit tests, including malformed and maximum inputs.
2. Add serialization/checksum and corruption tests for stateful contracts.
3. Add worker fixtures without live external providers.
4. Add cold/original and warm/transformed transaction tests.
5. Extend all three surface smoke suites.
6. Validate native platform, package, container, upgrade, and lifecycle lanes.
7. Update product configuration/integration docs and monorepo internals.

Use fuzz targets for parsers and queue inputs, and use deterministic runtime comparison for executable JavaScript/CSS fixtures where possible.
