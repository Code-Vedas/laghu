---
title: Startup and Runtime
parent: Troubleshooting
grand_parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/troubleshooting/runtime/
---

# mod-laghu Startup and Runtime

## Module Loading

Confirm `LoadModule laghu_module` references the packaged module for the running Apache, APR, architecture, and MPM environment.
An undefined symbol or module-magic-number error requires a matched package or rebuild.

## Configuration and Inheritance

Use `apache2ctl -S` to inspect virtual-host selection and the configuration test to catch invalid scope, ranges, duplicate policy selectors, provider files, or process-wide RUM settings placed in a virtual host.
Directory and location policy can override inherited optimization settings, while backend settings remain main-server-only.

## Output Filter Is Not Applied

Confirm the request reaches the intended virtual host, `Laghu On` is effective, and another module has not already consumed or replaced the response unexpectedly.
Authorization, private caching, API exclusions, unsupported responses, malformed input, and never-larger rejection remain deliberate passthrough.

## Latency or Child Instability

Compare one narrow scope with `Laghu Off`, inspect MPM saturation, bucket sizes, cache storage latency, and upstream timing.
Codec, SWC, provider, and Redis work must remain outside Apache children; a dependency failure should appear as degraded optimization rather than blocked request threads.

| Symptom | Check | Expected recovery |
| --- | --- | --- |
| `Invalid command 'Laghu'` | Confirm `LoadModule` path and server ABI. | Install/rebuild the matched module and rerun the configuration test. |
| Configuration rejects a directive | Check scope, range, duplicate policy selectors, and referenced provider files. | Correct the configuration before restart. |
| Responses remain unchanged | Confirm `Laghu On`, eligible status/type/path, and response cache policy. | Preserve deliberate exclusions; enable only the intended scope. |
| Variants never become ready | Inspect worker services, queue permissions, and cache ownership for the Apache worker account. | Restore access and allow normal requests to republish. |
| Immutable routes return 404 | Confirm the catalog and payload were atomically published. | Repair cache access; never create route files manually. |
| RUM learning stays local | Inspect beacon CSP, snapshot access, hiredis loading, TLS trust, and backend health. | Request handling remains memory-backed while background retry continues. |

On systemd packages, inspect `laghu-libvips`, `laghu-resource-fetch`, and `laghu-js-optimize` service status. When HTML micro-caching is enabled, also inspect its matching `laghu-html-refresh@NAME` instance and ensure its queue, cache, and origin equal the Apache settings.
