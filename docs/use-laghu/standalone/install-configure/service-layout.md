---
title: Service layout
parent: Install and configure
grand_parent: Standalone
ancestor: Use Laghu
---
# Service layout

Create `/etc/laghu/laghu.yaml`, `/etc/laghu/conf.d`, `/run/laghu`, and `/var/cache/laghu/images`; the service account owns writable runtime paths. Use `runtime.cache: /var/cache/laghu/images` and `runtime.worker_queue: /run/laghu/jobs.queue`.

## Check

Follow the next page only after this step has completed without an installation or configuration error.
