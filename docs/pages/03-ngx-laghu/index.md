---
title: NGINX integration
nav_order: 4
permalink: /ngx-laghu/
---

# NGINX integration

`ngx-laghu` is the native NGINX adapter. Build it against the target NGINX source with `--add-dynamic-module`, load the resulting module, and enable `laghu` in the required NGINX scope.

The module uses normal NGINX inheritance and delegates policy, cache, and worker decisions to the shared engine. Configure worker queues and cache paths under the same service account as NGINX. See [Operations](/operations/) for common runtime behavior.

```sh
./configure --with-compat --add-dynamic-module=/path/to/laghu/modules/ngx_http_laghu_module
make modules
```
