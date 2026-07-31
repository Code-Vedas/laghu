---
title: Virtual Hosts and Rollout
parent: Guides
grand_parent: mod-laghu
nav_order: 2
permalink: /mod-laghu/guides/deployment/
---

# mod-laghu Virtual Hosts and Rollout

Keep process-wide queues, cache paths, provider files, JavaScript targets, and RUM backend settings in main server configuration.
Apply policies at virtual-host, directory, or location scope only where their inheritance is intentional.

For event, worker, and prefork MPMs, asynchronous optimization remains outside Apache children.
Size cache and worker services for aggregate host traffic rather than for one child process.

Roll out through a dedicated canary virtual host or a controlled traffic experiment, validate output filters alongside compression and proxy modules, then use a graceful restart after every successful configuration test.
When shared hosting is involved, use per-tenant policy and cache keys, restrict administration endpoints, and avoid allowing tenant content to alter provider or RUM backend configuration.

Containers should mount persistent cache/snapshot storage, use explicit UID/GID-compatible permissions, expose Apache and worker health independently, and keep Redis/Valkey outside the container lifecycle.
