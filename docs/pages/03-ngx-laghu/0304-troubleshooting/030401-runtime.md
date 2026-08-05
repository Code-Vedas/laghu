---
title: Startup and Runtime
parent: Troubleshooting
grand_parent: ngx-laghu
nav_order: 1
permalink: /ngx-laghu/troubleshooting/runtime/
---

# ngx-laghu Startup and Runtime

## Module Does Not Load

Run `nginx -V` and confirm the module was built for the same NGINX version, architecture, compatibility signature, compiler ABI, and enabled module set.
Confirm `load_module` appears at main scope before `events` and `http`.
An `Exec format error`, undefined symbol, or module incompatibility message requires the matched package; configuration changes cannot repair an ABI mismatch.

## Configuration Reload Fails

Run `nginx -t` as the same user and environment used by the service manager.
Check directive scope, duplicate settings, mutually exclusive policy selectors, numeric bounds, readable provider files, and environment-expanded Redis values.
The running workers retain the previous valid configuration when a reload is rejected.

## Requests Pass Through

Inspect `X-Laghu`, response cache/CSP headers, and authenticated metrics.
Authorization, `private`, `no-store`, unsupported status/content type, excluded API paths, malformed bodies, size limits, unready dependencies, and never-larger rejection are expected passthrough reasons.
Do not enable `allow_api` globally merely to hide an exclusion.

## NGINX Becomes Unhealthy

Laghu worker loss should not block the NGINX event loop.
If latency rises, check body-capture limits, disk latency, cache contention, configuration loops, and unrelated upstream behavior, then compare a canary location with `laghu off;`.
Capture `nginx -T`, authenticated metrics/readiness output, and representative response headers before escalation.

| Symptom | Check | Expected recovery |
| --- | --- | --- |
| `unknown directive "laghu"` | Confirm `load_module` points to the matched module and inspect `nginx -V`. | Install/rebuild for that NGINX ABI, then run `nginx -t`. |
| Configuration test rejects a setting | Check spelling, scope, range, duplicate selectors, and provider files. | Correct configuration; invalid configuration never reaches reload. |
| Every response passes through | Confirm `laghu on;`, eligible status/content type, cache policy, and path. | Narrowly enable the intended scope; authorization/private/API exclusions are deliberate. |
| Cold responses never become warm | Check worker service status, queue path, group permissions, and cache writability. | Restore the worker/path and request the resource again. |
| Immutable `/.laghu/` route returns 404 | Confirm the catalog entry and cached payload are ready and uncorrupted. | Allow normal traffic to republish; do not fabricate cache files. |
| RUM does not converge | Check beacon opt-in/CSP, snapshot directory, hiredis path, TLS trust, and backend availability. | Request traffic continues from memory; repair background storage and let the retained batch retry. |

Use `systemctl status laghu-libvips laghu-resource-fetch laghu-js-optimize` on systemd packages.
Laghu intentionally preserves original responses when a queue is full, a worker is absent, a candidate is invalid, or a transformation is not smaller.
