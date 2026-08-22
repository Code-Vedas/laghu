---
title: Apache integration
nav_order: 5
permalink: /mod-laghu/
---

# Apache integration

`mod_laghu` is the native Apache HTTP Server adapter. Load its module, enable `Laghu` in the intended Apache scope, and point it at the shared cache and worker queues.

Apache retains virtual-host configuration and bucket brigades; Laghu uses the same policy and runtime as NGINX and standalone. See [Operations](/operations/) for shared worker, cache, and administrative behavior.

```apache
LoadModule laghu_module modules/mod_laghu.so
Laghu On
```
