---
title: Repository Architecture
parent: Developer Guide
nav_order: 2
permalink: /developer-guide/repository/
---

# Repository Architecture

| Path | Ownership |
| --- | --- |
| `libs/laghu-core` | Shared configuration, policy, hashing, and final selection. |
| `libs/laghu-http` | Server-neutral HTTP transaction contract. |
| `libs/laghu-image` | Bounded image, markup, and CSS discovery logic. |
| `libs/laghu-runtime` | Catalogs, planners, queues, instrumentation, and RUM state. |
| `modules/ngx_http_laghu_module` | NGINX configuration and filter adapter. |
| `modules/mod_laghu` | Apache configuration and output-filter adapter. |
| `servers/laghu` | Standalone reverse proxy. |
| `workers/laghu-libvips` | Image encoding and sprite work. |
| `workers/laghu-resource-fetch` | Provider-configured font stylesheet fetching. |
| `workers/laghu-js-optimize` | Locked native SWC processing. |
| `packaging` | Package, service, container, Homebrew, and Windows definitions. |
| `scripts` | Canonical build and validation entry points. |

Adapters must not acquire codec or network dependencies.
Worker protocols are bounded and versioned, cache publication is atomic, and all consumers validate checksums and compatibility before use.

Configuration behavior begins in `laghu-core` but syntax and context belong to each product parser.
When adding a setting, update all supported surfaces, tests, examples, packages, and the three product configuration pages together.
