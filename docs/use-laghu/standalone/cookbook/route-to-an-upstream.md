---
title: How do I route to an upstream?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I route to an upstream?

Create the narrowest route or proxy rule first. Confirm the expected response and health endpoint before widening it; remove the rule to return traffic to the prior origin. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

~~~yaml
routes:
  - match: prefix
    pattern: /api/
    proxy_pass: http://127.0.0.1:9000
    health_check: /healthz
    health_interval: 5
~~~

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
curl -i http://127.0.0.1:8080/.laghu/ready
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
