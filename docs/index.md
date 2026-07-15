---
title: Home
nav_order: 1
permalink: /
---

# Laghu

Laghu is a native NGINX content-optimization module designed to make edge
responses smaller and faster without application changes.

The project is being built around four non-negotiable properties:

- **fail-open delivery:** optimizer failures preserve the original response
- **event-loop safety:** expensive work belongs outside NGINX workers
- **deterministic output:** identical inputs produce fleet-safe variants
- **measured value:** transforms ship with correctness, resource, and latency
  evidence

## Current Status

Laghu is an engineering skeleton, not a production optimizer. The current code
provides a buildable dynamic module, configuration inheritance, response
eligibility and preset policy, deterministic variant-key and candidate-safety
contracts, a pass-through filter seam, and an `X-Laghu` decision header.
Response bodies are not rewritten yet.

That distinction is intentional: the docs mark delivered behavior separately
from planned product capabilities.

## Start Here

- [Installation](/installation/) builds and loads the module.
- [Configuration](/configuration/) describes the implemented directives.
- [Architecture](/architecture/) explains the module/core/worker split.
- [Compatibility](/compatibility/) records the validated server matrix.
