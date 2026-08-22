---
title: How do I recover from a failed change?
parent: Cookbook and troubleshooting
grand_parent: Apache
ancestor: Use Laghu
---
# How do I recover from a failed change?

Run the native configuration check before reload. If traffic regresses, restore saved configuration, validate it, reload, and retain logs for diagnosis. Set `Laghu On` in the intended server or virtual-host scope after the queue and cache are ready.

## Verify

~~~sh
apachectl configtest && systemctl reload apache2
~~~

Expected result: the configuration validates or reloads without error. If it does not, restore the last working configuration before another change.

## Evidence to keep

Keep validation output, relevant server and worker logs, and the request URL used for the check.
