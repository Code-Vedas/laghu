---
title: How do I enable Laghu safely?
parent: Cookbook and troubleshooting
grand_parent: NGINX
ancestor: Use Laghu
---
# How do I enable Laghu safely?

Leave Laghu disabled until the cache and worker queue are writable. Enable it for one site, request known HTML, and retain the previous configuration for rollback. Set `laghu on;` in the intended scope after the queue and cache are ready.

## Verify

~~~sh
nginx -t && systemctl reload nginx
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
