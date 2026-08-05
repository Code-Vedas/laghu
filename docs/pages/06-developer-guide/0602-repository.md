---
title: Repository Architecture
parent: Developer Guide
nav_order: 2
permalink: /developer-guide/repository/
---

# Repository Architecture

| Path | Ownership |
| --- | --- |
| `libs/laghu-base` | Dependency-free bounded buffers, ASCII, hashing, numbers, and URLs. |
| `libs/laghu-markup` | Bounded HTML, attribute, `srcset`, and resource tokenization. |
| `libs/laghu-core` | Shared configuration schema, resolved policy, eligibility, and final selection. |
| `libs/laghu-http` | Complete server-neutral HTTP transaction and fail-open result ownership. |
| `libs/laghu-image` | Image decoding, transforms, probes, and image-markup decisions. |
| `libs/laghu-runtime` | Caches, catalogs, queues, transformation services, instrumentation, and RUM state. |
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

Configuration identity, types, bounds, defaults, conflicts, and semantic validation belong to `laghu-core`; native syntax and context belong to each product adapter.
When adding a setting, update all supported surfaces, tests, examples, packages, and the three product configuration pages together.

`cmake/laghu-sources.list` is the canonical native source inventory consumed by CMake and native module/package builds.
