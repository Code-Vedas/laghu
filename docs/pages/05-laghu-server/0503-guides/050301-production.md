---
title: Production Recommendations
parent: Guides
grand_parent: Laghu Server
nav_order: 1
permalink: /laghu-server/guides/production/
---

# Laghu Server Production Recommendations

1. Bind a private listener behind the public ingress unless Laghu itself is intentionally the edge process.
2. Use a verified HTTPS origin and an administrator-owned CA bundle only when platform trust is insufficient.
3. Enable forwarded headers only with explicit canonical trusted-proxy CIDRs.
4. Size the worker and connection queues within host memory and origin concurrency limits.
5. Start optimization workers before the proxy and use persistent queue, cache, and snapshot directories.
6. Use a durable independently operated Redis/Valkey service when learning must survive a fleet replacement.
7. Exercise graceful termination and verify the configured drain timeout before production rollout.
8. Keep beaconing opt-in and review CSP, sampling, retention, and privacy first.

The current server handles HTTP/1.1 and one configured origin.
Persistent origin pooling and downstream TLS termination remain roadmap work and must not be assumed.
