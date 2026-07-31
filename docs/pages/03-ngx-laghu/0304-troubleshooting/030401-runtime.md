---
title: Startup and Runtime
parent: Troubleshooting
grand_parent: ngx-laghu
nav_order: 1
permalink: /ngx-laghu/troubleshooting/runtime/
---

# ngx-laghu Startup and Runtime

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
