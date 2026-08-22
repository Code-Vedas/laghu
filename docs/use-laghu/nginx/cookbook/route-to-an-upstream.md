---
title: How do I route to an upstream?
parent: Cookbook and troubleshooting
grand_parent: NGINX
ancestor: Use Laghu
---
# How do I route to an upstream?

Create the narrowest route or proxy rule first. Confirm the expected response and health endpoint before widening it; remove the rule to return traffic to the prior origin. Set `laghu on;` in the intended scope after the queue and cache are ready.

~~~nginx
location /api/ {
  proxy_pass http://127.0.0.1:9000;
  laghu on;
}
~~~

## Verify

~~~sh
nginx -t && systemctl reload nginx
curl -i http://127.0.0.1/.laghu/ready
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
