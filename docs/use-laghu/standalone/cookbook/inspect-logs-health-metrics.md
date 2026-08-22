---
title: How do I inspect logs, health, and metrics?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I inspect logs, health, and metrics?

Use `/.laghu/ready` for readiness and `/.laghu/stats` for cache statistics, then inspect server and worker logs. Preserve timestamps before changing state. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

~~~sh
curl -fsS http://127.0.0.1:8080/.laghu/ready
curl -fsS http://127.0.0.1:8080/.laghu/stats
~~~

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
