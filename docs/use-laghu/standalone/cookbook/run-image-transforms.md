---
title: How do I run image transforms?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I run image transforms?

Enable image policy only after a worker consumes its queue. Request one image twice and inspect output and cache state; disable policy if the original cannot be served. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
