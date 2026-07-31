---
title: Workers, Queues, and Cache
parent: Troubleshooting
grand_parent: Laghu Server
nav_order: 2
permalink: /laghu-server/troubleshooting/workers-and-cache/
---

# Laghu Server Workers, Queues, and Cache

Use `laghu status` to compare request workers, optimization-worker heartbeat, queue versions/capabilities, queue depth, job age, cache availability, and free space.

If responses never warm, verify the server and workers use identical queue/cache paths and compatible protocol versions.
If queues saturate, preserve the original response, identify the expensive job class, and scale its worker independently.

For checksum failure, missing immutable routes, or stale variants, use `laghu explain` and targeted `laghu purge`.
Allow ordinary origin traffic to recreate state; avoid deleting a mounted cache during rolling deployment.

Persistent cache latency directly affects warm reads and publication.
Monitor p95/p99 I/O, inode pressure, eviction, and variant fanout before increasing limits.
