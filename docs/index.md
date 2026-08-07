---
title: Introduction
nav_order: 1
permalink: /introduction/
---

# Introduction

Laghu is a production content-optimization platform for NGINX, Apache HTTP Server, and standalone reverse-proxy deployments.
It rewrites HTML, CSS, JavaScript, images, fonts, cache policy, and resource hints at the delivery layer, allowing teams to improve page weight and Core Web Vitals without coupling optimization logic to every application.

Laghu is delivered as three products backed by one optimization engine:

| Product | Integration model | Best fit |
| --- | --- | --- |
| [`ngx-laghu`](/ngx-laghu/) | Native NGINX module | NGINX ingress, reverse proxy, origin, or static server |
| [`mod-laghu`](/mod-laghu/) | Native Apache 2.4 module | Apache virtual hosts, shared hosting, and application frontends |
| [`laghu` server](/laghu-server/) | Standalone reverse proxy | Origins that cannot load a native module or need an independent optimization tier |

## Why Laghu Exists

Modern web performance is distributed across markup structure, image formats, responsive variants, JavaScript execution, CSS delivery, font loading, cache behavior, and runtime conditions that differ by template and viewport.
Application teams can solve these concerns individually, but the result is often duplicated build tooling, inconsistent policies, stale generated assets, and no common operational view.

Laghu moves that work into a shared delivery system with four design commitments:

- **fail-open delivery:** an unavailable worker, cache, learning backend, or unsupported transformation preserves the origin response;
- **bounded execution:** parsers, queues, bodies, dependency graphs, caches, retries, and runtime observations have explicit limits;
- **asynchronous optimization:** codecs, SWC, external provider access, and fleet synchronization stay outside request-critical server threads;
- **evidence-driven decisions:** RUM observations are aggregated by opaque template and viewport keys and can inform critical CSS, images, JavaScript deferral, and optimization profiles.

## What Laghu Optimizes

### Images

Laghu produces validated AVIF, WebP, JPEG, PNG, GIF, SVG, and responsive viewport/DPR variants; applies content-aware quality policy; preserves animation when appropriate; emits dimensions and responsive markup; inlines small assets; combines suitable icons; and prioritizes learned LCP images.

### CSS and Fonts

Laghu minifies and combines stylesheets, rewrites ready dependencies, flattens imports, inlines or outlines by policy, learns critical rules by template and viewport, preserves cascade and CSP behavior, and inlines configured external font CSS from built-in or administrator-defined providers.

### JavaScript

Laghu uses native pinned SWC builds to parse classic and module scripts, lower syntax for configured Browserslist targets, minify, combine explicit safe groups, inline or outline eligible code, preserve source maps, and defer non-critical execution from privacy-preserving RUM evidence.

### HTML, Caching, and Delivery

Laghu normalizes safe HTML syntax, injects permitted resource hints and instrumentation, rewrites cacheable resources to immutable content hashes, extends cache policy, supports domain mapping, serves pre-compressed text variants, emits Early Hints, and preserves CSP, `Vary`, validators, and origin semantics.

## How Optimization Reaches Production

The first eligible response is normally delivered unchanged while Laghu publishes bounded asynchronous work.
Workers validate and atomically publish derived content into the immutable cache.
Subsequent requests use dependency-aware keys and validators to select ready output.
No page is made dependent on a successful optimization job.

Browser instrumentation is opt-in.
It reports bounded anonymous observations rather than page text, selectors, complete URLs, cookies, addresses, or client identifiers.
Every request worker reads RUM decisions from memory; local snapshots or Redis/Valkey synchronize learning in the background.

## Choosing a Deployment

Choose a native module when the web server ABI and lifecycle are under your control.
Choose the standalone server when an application platform cannot load native code, when several origins share an optimization tier, or when optimization should scale independently from the origin.
All products expose the same policy model, immutable routes, observability contract, purge behavior, and fail-open guarantees.

## Operating Model

Production installations include the selected adapter or server, `laghu-libvips`, `laghu-resource-fetch`, `laghu-js-optimize`, and—when HTML micro-caching is enabled—a per-origin `laghu-html-refresh` worker, provider configuration, runtime directories, health probes, service lifecycle, and package-native upgrade handling.
Operators can explicitly enable authenticated Prometheus metrics, aggregate readiness, cache statistics, and purge controls without editing application code.

Start with [Architecture](/architecture/), then follow the installation and integration guide for your selected product.

## License and Support

Laghu is complete open-source software under the [MIT license](https://github.com/Code-Vedas/laghu/blob/main/LICENSE).
It has no license key, paid feature gate, hosted-service requirement, or open-core boundary.
[Professional support](/professional-support/) is optional and does not change product functionality.
