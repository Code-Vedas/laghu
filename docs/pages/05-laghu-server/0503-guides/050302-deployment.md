---
title: Ingress, Origin, and Rollout
parent: Guides
grand_parent: Laghu Server
nav_order: 2
permalink: /laghu-server/guides/deployment/
---

# Ingress, Origin, and Rollout

Place Laghu behind a load balancer or run it as the edge listener with downstream TLS and HTTP/2/HTTP/3 enabled.
Configure one or more explicit origins through the supported routing/domain map and restrict forwarded identity to canonical trusted-proxy networks.

Use separate health signals for downstream acceptance, origin connectivity, worker queues, cache capacity, and RUM synchronization.
Readiness should fail only for dependencies required to serve correct traffic; optimization-only failures should remain degraded and fail open.

Scale request workers for concurrent client/origin work, scale optimization workers for queued CPU cost, and place immutable cache on persistent low-latency storage.
Use Redis/Valkey to synchronize learning, not as a per-request cache lookup.

Roll out a new package with a canary pool, warm representative templates, compare response headers, metrics, and Core Web Vitals, then perform a connection-draining rolling replacement.
