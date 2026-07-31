---
title: Startup and Runtime
parent: Troubleshooting
grand_parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/troubleshooting/runtime/
---

# mod-laghu Startup and Runtime

| Symptom | Check | Expected recovery |
| --- | --- | --- |
| `Invalid command 'Laghu'` | Confirm `LoadModule` path and server ABI. | Install/rebuild the matched module and rerun the configuration test. |
| Configuration rejects a directive | Check scope, range, duplicate policy selectors, and referenced provider files. | Correct the configuration before restart. |
| Responses remain unchanged | Confirm `Laghu On`, eligible status/type/path, and response cache policy. | Preserve deliberate exclusions; enable only the intended scope. |
| Variants never become ready | Inspect worker services, queue permissions, and cache ownership for the Apache worker account. | Restore access and allow normal requests to republish. |
| Immutable routes return 404 | Confirm the catalog and payload were atomically published. | Repair cache access; never create route files manually. |
| RUM learning stays local | Inspect beacon CSP, snapshot access, hiredis loading, TLS trust, and backend health. | Request handling remains memory-backed while background retry continues. |

On systemd packages, inspect `laghu-libvips`, `laghu-resource-fetch`, and `laghu-js-optimize` service status.
