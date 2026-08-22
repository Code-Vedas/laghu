---
title: How do I collect RUM?
parent: Cookbook and troubleshooting
grand_parent: NGINX
ancestor: Use Laghu
---
# How do I collect RUM?

Use the local store first and set bounded memory, pending, and TTL limits. Confirm a beacon reaches the endpoint; disable collection when consent or retention policy is not met. Set `laghu on;` in the intended scope after the queue and cache are ready.

## Verify

~~~sh
nginx -t && systemctl reload nginx
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
