---
title: Deployment and Rollout
parent: Guides
grand_parent: ngx-laghu
nav_order: 2
permalink: /ngx-laghu/guides/deployment/
---

# ngx-laghu Deployment and Rollout

## Recommended Topology

Run one set of optimization workers per host, share their bounded queues with all NGINX workers, keep immutable cache storage local or on a low-latency persistent volume, and use Redis/Valkey only to synchronize RUM aggregates and learned decisions.
Do not put Redis, provider fetching, or codec work on the NGINX event loop.

## Safe Rollout

1. Install the matched module and workers while Laghu remains off.
2. Validate provider files, JavaScript targets, queue ownership, cache space, snapshot permissions, and TLS trust before starting the service.
3. Enable `safe` or `balanced` on a canary location.
4. Warm representative pages through ordinary traffic and compare headers, bytes, visual output, CSP, and application behavior.
5. Enable opt-in instrumentation and wait for template buckets to reach readiness.
6. Expand by server group while monitoring passthrough reasons, queue pressure, cache growth, error rate, and Core Web Vitals.
7. Retain the previous module/package and configuration for immediate rollback.

NGINX reload keeps established connections while new workers load the new configuration.
Configuration digests in provider, queue, catalog, policy, and RUM records prevent an old worker from silently widening access or serving incompatible output.

## Kubernetes and Containers

Mount `/var/cache/laghu/images` and `/var/lib/laghu/rum` on persistent volumes when warm state must survive pod replacement.
Expose worker and adapter readiness separately, use graceful termination long enough for active requests, and place Redis/Valkey outside the disposable Laghu pod lifecycle.
Run rootless with explicit group ownership for queue and cache mounts.
