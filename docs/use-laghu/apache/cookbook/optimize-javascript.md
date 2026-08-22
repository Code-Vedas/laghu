---
title: How do I optimize JavaScript?
parent: Cookbook and troubleshooting
grand_parent: Apache
ancestor: Use Laghu
---
# How do I optimize JavaScript?

Start with observation, validate one script’s browser behavior, then enable optimization. Revert the per-site policy if the optimized asset changes runtime behavior. Set `Laghu On` in the intended server or virtual-host scope after the queue and cache are ready.

## Verify

~~~sh
apachectl configtest && systemctl reload apache2
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
