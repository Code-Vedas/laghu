---
title: How do I inspect logs, health, and metrics?
parent: Cookbook and troubleshooting
grand_parent: Apache
ancestor: Use Laghu
---
# How do I inspect logs, health, and metrics?

Use `/.laghu/ready` for readiness and `/.laghu/stats` for cache statistics, then inspect server and worker logs. Preserve timestamps before changing state. Set `Laghu On` in the intended server or virtual-host scope after the queue and cache are ready.

~~~sh
curl -fsS http://127.0.0.1/.laghu/ready
curl -fsS http://127.0.0.1/.laghu/stats
~~~

## Verify

~~~sh
apachectl configtest && systemctl reload apache2
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
