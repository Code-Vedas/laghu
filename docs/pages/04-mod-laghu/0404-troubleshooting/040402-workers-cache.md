---
title: Workers, Queues, and Cache
parent: Troubleshooting
grand_parent: mod-laghu
nav_order: 2
permalink: /mod-laghu/troubleshooting/workers-and-cache/
---

# mod-laghu Workers, Queues, and Cache

Confirm worker services, queue protocol probes, Apache group membership, directory traversal permissions, free bytes/inodes, and atomic rename support.
Apache child accounts must access the same queue and immutable cache paths configured in main server or inherited policy.

Queue saturation preserves the original response.
Measure publication rate, worker CPU and memory, repeated failures, and job latency before changing capacity.

For cache corruption or immutable-route 404s, use `laghu explain` and `laghu purge` for the affected resource, then allow ordinary traffic to republish.
Do not remove the entire shared cache while Apache children or workers are active.

If only one virtual host fails to warm, compare its effective `WorkerQueue`, `ImageCache`, policy key, response validators, and filesystem permissions with a healthy host.
