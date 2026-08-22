---
title: Standalone server
nav_order: 6
permalink: /laghu-server/
---

# Standalone server

Laghu standalone is a mature deployment surface for independent scaling, YAML configuration, and modern delivery capabilities. It is a peer to the NGINX and Apache integrations, not a fallback: use it where an independent optimization tier or explicit service configuration is the right operational boundary.

Copy `servers/laghu/laghu.yaml.example`, set the listener, origin, cache, queues, and TLS paths, then start `laghu --config /etc/laghu/laghu.yaml`. It uses the same shared policy, workers, cache, and fail-open behavior as the native adapters.

```sh
laghu --config /etc/laghu/laghu.yaml
```
