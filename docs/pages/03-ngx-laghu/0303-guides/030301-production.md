---
title: Production Recommendations
parent: Guides
grand_parent: ngx-laghu
nav_order: 1
permalink: /ngx-laghu/guides/production/
---

# ngx-laghu Production Recommendations

1. Install a module matched to the running NGINX ABI and validate `nginx -t`.
2. Keep `laghu off;` while verifying queue, cache, provider-file, and snapshot permissions.
3. Start `laghu-libvips`, `laghu-resource-fetch`, and `laghu-js-optimize` before enabling their filters.
4. Enable `balanced` in a narrow `server` or `location` scope and confirm cold passthrough followed by warm variants.
5. Keep `/var/cache/laghu/images` persistent when variants must survive replacement.
6. Keep `/var/lib/laghu/rum` persistent for single-host learning, or select a separately operated durable Redis/Valkey service with verified TLS.
7. Leave beacon features off until privacy, CSP, retention, and sampling have been reviewed.
8. Reload only after `nginx -t`; worker or backend loss after startup remains fail open unless RUM storage is marked required.

Do not place remote Redis credentials in committed NGINX configuration.
Use administrator-owned environment expansion and ensure normal diagnostics cannot expose the resolved URI.
