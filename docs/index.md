---
title: Introduction
nav_order: 1
permalink: /introduction/
---

# Introduction

Laghu provides native content-optimization modules for NGINX and Apache HTTP Server plus a standalone reverse proxy for any HTTP origin, making edge responses smaller without application changes.

## Laghu

Laghu applies one shared optimization policy through three user-facing products. Expensive image, JavaScript, and external-resource work runs outside request-serving processes; an unavailable optimizer preserves the origin response.

## Why Laghu?

Laghu restores install-once server-side optimization on a maintained architecture for current web servers. It is not a source port of an older optimizer and does not expose legacy product names or directives.

## Choose a Product

| Product | Use it when |
| --- | --- |
| [`ngx-laghu`](/ngx-laghu/) | NGINX can load a matched native module. |
| [`mod-laghu`](/mod-laghu/) | Apache HTTP Server 2.4 can load a matched native module. |
| [`laghu` server](/laghu-server/) | The origin cannot load either native module. |

All three use the same policy, catalogs, immutable cache, worker protocols, and fail-open rules. Native modules are bound to their target server ABI; the standalone server is a separate reverse proxy.

## Runtime Guarantees

- **fail-open delivery:** service failures preserve the original response
- **server safety:** expensive work remains outside web-server processes
- **deterministic output:** identical inputs produce fleet-safe variants
- **executable validation:** completed transforms include direct and server-level correctness tests

## License

Laghu is complete open-source software distributed under the [MIT license](https://github.com/Code-Vedas/laghu/blob/main/LICENSE). It has no license key, paid feature gate, hosted-service requirement, or open-core boundary.

Continue with [Architecture](/architecture/) or select a product above.
