---
title: How do I serve static content?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I serve static content?

Set a document root and bounded static response size. Request a known asset and range response; if either is wrong, remove the static policy and keep the origin path. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
