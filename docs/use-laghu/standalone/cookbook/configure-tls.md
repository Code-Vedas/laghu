---
title: How do I configure TLS?
parent: Cookbook and troubleshooting
grand_parent: Standalone
ancestor: Use Laghu
---
# How do I configure TLS?

Install certificate and key files readable only by the service account, validate configuration, then test SNI and the chain. Restore the prior pair on failure. Run with `/etc/laghu/laghu.yaml`; keep overrides in lexical `/etc/laghu/conf.d/*.yaml` fragments.

## Verify

~~~sh
laghu --config /etc/laghu/laghu.yaml
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
