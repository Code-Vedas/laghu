---
title: Optimization Pipelines
parent: Developer Guide
nav_order: 3
permalink: /developer-guide/optimizations/
---

# Optimization Pipelines

This page orients contributors to the implementation path for optimization work.
It is not an application-integration guide.

## Common Lifecycle

1. An adapter normalizes an eligible response into the bounded HTTP contract.
2. The shared runtime consults checksummed catalogs and in-memory learning state.
3. Cold content is delivered unchanged while one non-blocking job may be published.
4. A worker parses and derives a candidate outside the request-serving process.
5. Only valid, non-identical, strictly smaller output is atomically published, except for explicitly bounded instrumentation or critical-CSS HTML growth.
6. A later request may serve the ready variant with dependency-derived validators.

The implementation spans `laghu-core` policy, `laghu-http` transaction orchestration, `laghu-runtime` catalogs/planners, surface adapters, worker protocols, and cross-surface smoke fixtures.
A change is incomplete until those owners agree on versioning, bounds, fail-open behavior, and validation.

## Images

`laghu-libvips` detects supported input by magic, uses explicit loaders, classifies a bounded 64px decoded thumbnail, and applies fixed photo,
screenshot, illustration, or flat-color quality caps before publication.
`Save-Data: on` lowers that selected cap. Mobile (≤767px), tablet (768–1199px), desktop (≥1200px), format, and data-saver
representations use separate identities. Explicit `image_quality` remains an upper bound; images never enlarge or upscale.
Safe static SVGs are optimized in the shared HTTP path by removing comments, metadata, and Inkscape/Sodipodi editor attributes after
rejecting scripts, event handlers, external references, entities, and unsupported constructs. Rejected SVGs pass through unchanged.
HTML discovery does not fetch images; normal image traffic populates catalogs.
Critical-image learning uses opaque viewport-bucket observations and can inform dimensions, responsive variants, and inlining within configured bounds.

## CSS and Fonts

The dependency-free tokenizer bounds input, tokens, URLs, nesting, and sprite inputs while preserving strings, custom properties, calculation spacing, legal comments, and source directives.
CSS transformation covers safe minification, image URL rewriting, combination, inline/outline behavior, import planning, style attributes, placement, and learned critical CSS.

Critical CSS learns four fixed evidence buckets: mobile/desktop crossed with light/dark. The beacon ignores inactive media queries and reports the active color-scheme bucket. Laghu unions proven light and dark rules for the selected viewport and always retains complete `prefers-color-scheme` blocks, custom-property and `color-scheme` declarations, font/keyframe dependencies, and recognized root theme selectors. HTML/body class, `data-theme`, and `data-color-scheme` state affects only the opaque template fingerprint; raw theme values are not persisted or exposed. Missing or conflicting theme evidence preserves the original stylesheet arrangement.

Every request uses a non-blocking transform budget. Content ceilings, temporary memory, generated output, deterministic work, deadline checkpoints, dependencies, and variant fanout are bounded. Exhausting one budget skips the affected optimization and preserves response delivery. Cache publication reserves bounded metadata before writing, respects byte/inode/metadata limits, and refuses excess variants for a canonical source.

Operational output classifies skips in `laghu_transform_rejections_total` with only the fixed `content`, `memory`, `deadline`, `cache`, and `variants` reasons. `laghu_transform_memory_bytes`, `laghu_transform_deadline_milliseconds`, `laghu_cache_rejected_writes_total`, `laghu_variant_occupancy`, and `laghu_variant_limit` expose bounded aggregate state without source URLs, selectors, paths, keys, or theme values. Budget pressure is diagnostic and does not make readiness fail.

External font CSS uses a separate provider configuration and `laghu-resource-fetch` queue.
The worker permits verified HTTPS GETs only, revalidates DNS and redirects, accepts font-only CSS, and never downloads or rewrites font binaries.
When a configured provider stylesheet is safely inlined, Laghu adds `font-display: swap` to each validated face and emits at most two provider-allowlisted WOFF/WOFF2 preload headers with `crossorigin`; unsupported, stale, CSP-blocked, or oversized input remains unchanged.

## HTML

HTML minification includes bounded resource URL trimming. The planner considers only same-origin script, stylesheet, preload, image, media, track, manifest, and icon attributes; navigation, forms, embedded documents, metadata redirects, and application URL schemes remain unchanged.
It resolves the original and every shorter root-relative or document-relative candidate against the first effective `<base href>` and accepts a replacement only when both absolute results are identical. Queries, fragments, percent encoding, path case, ports, and trailing slashes are preserved, while malformed or ambiguous URL and `srcset` input fails open.

### HTML Micro-cache and Revalidation

HTML micro-caching is off until an operator configures matching file-cache, `html_cache_origin`, `html_cache_ttl`, `html_cache_stale_ttl`, and `html_refresh_queue` values. Only anonymous `GET` HTML requests can use it; requests carrying `Authorization` or `Cookie` always pass to the origin. The cache key is the configured HTTPS origin plus the complete request path and query, so a client-controlled host never selects the refresh target.

A fresh entry is served immediately. During `html_cache_stale_ttl`, Laghu serves the last valid entry and publishes at most one bounded refresh job; `laghu-html-refresh` revalidates against the configured HTTPS origin with `If-None-Match`, preserves a `304`, and atomically replaces the entry only with a valid cacheable HTML `200`. A miss, a full queue, an invalid response, a DNS/TLS failure, or a stopped worker falls through to normal origin delivery.

Run one refresh-worker instance per origin and queue. It requires verified public HTTPS and rejects origins with paths, ports, credentials, queries, or fragments. The worker uses POSIX networking and service primitives; Windows package and service support are not provided or validated.

## JavaScript

`laghu-js-optimize` builds pinned SWC crates from `Cargo.lock` without Node or network access.
It parses classic and module programs separately, applies bounded Browserslist syntax lowering without polyfills/module conversion, compresses with unsafe transforms disabled, and preserves top-level bindings and property names.
The runtime observes same-origin external scripts through ordinary traffic and applies ready rewrite, combine, inline, or outline variants only when SWC metadata and CSP rules prove eligibility.
Deferral is stricter: RUM needs 100 fresh template-and-viewport observations with at least 90 percent candidate coverage, SWC must certify a classic program against parser-sensitive APIs, and an administrator must approve the exact query-free same-origin path. Approval never bypasses stale evidence, unsafe attributes, or ordering checks; removing an approval and reloading is the rollback path.
The planner defers only a complete approved suffix of blocking classic scripts, so an inline, unapproved, module, or incompatible later script keeps the preceding segment unchanged. Baseline and post-enable template generations remain separate; after 50 post-enable observations the runtime emits a bounded rollback recommendation when error/rejection rates, Core Web Vitals histograms, or lifecycle averages cross conservative thresholds.
When source maps are enabled, SWC publishes a version 3 map without `sourcesContent` and appends an immutable same-origin `sourceMappingURL` only while the resulting JavaScript remains strictly smaller. Map failure does not invalidate an otherwise safe optimized script.

## Instrumentation and Critical Learning

Opt-in same-origin scripts submit bounded opaque reports for critical images, critical CSS, and aggregate Core Web Vitals.
The server validates template and candidate keys against its own catalog and stores aggregate records only.
Cold, expired, malformed, CSP-blocked, or insufficiently observed pages remain unchanged.

## RUM State

Each request worker reads and merges an in-process RUM engine only.
A background synchronization thread rotates bounded pending deltas to a local snapshot or Redis/Valkey and reconciles returned aggregates into memory.
Redis/Valkey uses one versioned atomic Lua merge contract with idempotent batch markers; backend failure retains pending deltas and the last in-memory view.
