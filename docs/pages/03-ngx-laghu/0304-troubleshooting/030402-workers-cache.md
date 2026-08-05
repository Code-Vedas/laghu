---
title: Workers, Queues, and Cache
parent: Troubleshooting
grand_parent: ngx-laghu
nav_order: 2
permalink: /ngx-laghu/troubleshooting/workers-and-cache/
---

# ngx-laghu Workers, Queues, and Cache

## No Warm Variants

Confirm all three services are active, their queue probes report compatible protocol/capability versions, and the NGINX and service accounts share the expected `laghu` group.
Verify queue files under `/run/laghu`, cache space/inodes under `/var/cache/laghu/images`, and that the worker can atomically create and rename files on that filesystem.

## Queue Saturation

Queue-full diagnostics mean requests are correctly failing open.
Inspect job rate, worker CPU, timeouts, repeated invalid sources, and queue capacity before increasing bounds.
Scale workers or reduce enabled high-cost filters; never place blocking publication on the NGINX path.

## Cache Corruption or Unexpected 404

Use response headers and bounded operational metrics to identify the failure class, then verify the object exists, checksum matches, content type is correct, and the catalog version is supported.
Purge the affected URL or hash through `laghu purge`; do not recursively delete a shared cache while workers are publishing.
Ordinary origin traffic will republish missing derived content.

## Disk Pressure

Inspect cache metrics, variant fanout, inode usage, eviction, and purge history.
Reduce retained variants or cache bounds before the filesystem fills, and keep queue/snapshot paths on storage with their required atomic rename and locking semantics.
