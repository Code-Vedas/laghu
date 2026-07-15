---
title: Home
nav_order: 1
permalink: /
---

# Laghu

Laghu provides equal native content-optimization adapters for NGINX and Apache
HTTP Server, making edge responses smaller without application changes.

The project is being built around four non-negotiable properties:

- **fail-open delivery:** service failures preserve the original response
- **server safety:** expensive work remains outside web-server processes
- **deterministic output:** identical inputs produce fleet-safe variants
- **measured value:** transforms ship with correctness, resource, and latency
  evidence

## Current Status

Laghu now has a production-shaped image lane: each adapter streams cold
originals into a try-only bounded queue, the isolated libvips service validates
and atomically publishes smaller variants, and strong-validator warm requests
can deliver them. Missing codecs, worker loss, queue contention, malformed
input, deadlines, and cache failures preserve the original.

The broader HTML, CSS, JavaScript, administration, and cache-management surface
is still being built. Documentation claims only behavior with executable
coverage.

## Start Here

- [Installation](/installation/) builds and loads the module.
- [Configuration](/configuration/) describes the implemented directives.
- [Architecture](/architecture/) explains the module/core/worker split.
- [Compatibility](/compatibility/) records the validated server matrix.
