---
title: How do I diagnose cache misses?
parent: Cookbook and troubleshooting
grand_parent: NGINX
ancestor: Use Laghu
---
# How do I diagnose cache misses?

Check cache path, permissions, free space, and `/.laghu/stats`. Compare two identical requests; repair the cache path before changing policy. Set `laghu on;` in the intended scope after the queue and cache are ready.

## Verify

~~~sh
nginx -t && systemctl reload nginx
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
