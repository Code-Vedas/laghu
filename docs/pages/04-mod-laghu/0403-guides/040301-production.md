---
title: Production Recommendations
parent: Guides
grand_parent: mod-laghu
nav_order: 1
permalink: /mod-laghu/guides/production/
---

# mod-laghu Production Recommendations

1. Load a module built for the target Apache/APR environment and run `apache2ctl configtest` or `httpd -t`.
2. Leave `Laghu Off` while verifying worker services, queue ownership, cache permissions, and provider files.
3. Put process-wide RUM settings in main server configuration, not a virtual host or directory.
4. Enable `balanced` in one virtual host and verify cold passthrough followed by warm delivery.
5. Persist cache and snapshot directories across replacement when warm variants and learning must survive.
6. Use a separately operated Redis/Valkey backend with verified TLS when multiple servers must share learning.
7. Enable browser beacons only after privacy, CSP, and retention review.
8. Revalidate configuration before graceful restart.

Worker or optional RUM-backend loss preserves normal Apache delivery.
Required RUM mode intentionally makes initialization failure fatal.
