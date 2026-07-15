---
title: Architecture
nav_order: 4
permalink: /architecture/
has_children: true
---

# Architecture

Laghu separates server integration from optimization policy and expensive
execution.

```text
NGINX response filters
        |
        v
ngx_http_laghu_module  ---> original response (always available)
        |
        v
laghu-core policy      ---> decision, cache key, transform plan
        |
        v
optimization worker   ---> deterministic variant cache (planned)
```

## NGINX Module

The module owns directive parsing, configuration inheritance, response filter
registration, NGINX memory-pool use, and delivery of the original chain. It must
not perform blocking network or codec work in the event loop.

## Core Library

The C library owns policy that can be shared by the module, worker, CLI, and
sidecar. Its current responsibilities are configuration merge, preset-policy
resolution, conservative response eligibility, SHA-256 content and variant
keys, and final acceptance or rejection of transform candidates.

The candidate gate preserves a borrowed view of the original and selects
optimized output only when the producer reports it valid and it is strictly
smaller and non-identical. The caller owns both buffers and must keep their
storage alive while the result is used. Equal, larger, invalid, or failed
candidates resolve to the original. Variant keys use a versioned canonical
encoding of the original content and resolved policy, so the same input and
policy produce the same fleet-safe key.

## Worker and Cache

The planned worker performs expensive transforms asynchronously. A first cache
miss serves the original response; a successful optimization writes a
content-addressed variant for later requests. Worker absence, timeouts, parse
errors, and codec failures all preserve the original.

The shared cache contract must provide deterministic keys, bounded storage,
atomic publication, per-URL purge, and enough metadata for an explain surface.

## Current Request Path

The scaffold installs header and body filters. The header filter asks
`laghu-core` whether a response is eligible and records the decision. The body
filter forwards the original chain directly to the next NGINX filter.

No body buffering or mutation occurs.

The current path is therefore idempotent and reversible: it always forwards the
same original chain. Future transform, worker, and cache implementations must
use the core candidate and key contracts before joining this path.
