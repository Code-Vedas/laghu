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

For the native administration surface, send `PURGE /path?source-query` or an enabled `GET /path?source-query&laghu=purge` with `X-Laghu-Purge-Token` from an allowed direct peer. `202` means visibility changed, including an idempotent zero-match purge. `429` means the bounded tombstone table is full; allow maintenance to reclaim stale entries. Inspect protected `/.laghu/stats` for generation, purge, rejection, eviction, cleaner, and rebuild counters. A full flush file contains exactly `laghu-cache-flush-v1 GENERATION` followed by a newline; generations must increase.

Persistent cache latency directly affects warm reads and publication.
Monitor p95/p99 I/O, inode pressure, eviction, and variant fanout before increasing limits.
