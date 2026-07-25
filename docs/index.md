---
title: Home
nav_order: 1
permalink: /
---

# Laghu

Laghu provides native content-optimization modules for NGINX and Apache HTTP Server plus a standalone reverse proxy for any HTTP origin, making edge responses smaller without application changes.

Laghu enforces four runtime properties:

- **fail-open delivery:** service failures preserve the original response
- **server safety:** expensive work remains outside web-server processes
- **deterministic output:** identical inputs produce fleet-safe variants
- **executable validation:** completed transforms include direct and server-level correctness tests

## Runtime Behavior

Each adapter submits cold image responses to a try-only bounded queue. The isolated libvips service validates and atomically publishes smaller variants, and strong-validator warm requests can deliver them. Missing codecs, worker loss, queue contention, malformed input, deadlines, and cache failures preserve the original.

## Start Here

- [Installation](/installation/) builds and loads the module.
- [Standalone Proxy](/standalone-proxy/) configures HTTP origin forwarding.
- [Configuration](/configuration/) describes the implemented directives.
- [Architecture](/architecture/) explains the module/core/worker split.
- [Compatibility](/compatibility/) records the validated server matrix.
