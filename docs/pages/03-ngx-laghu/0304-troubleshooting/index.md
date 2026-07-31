---
title: Troubleshooting
parent: ngx-laghu
nav_order: 4
has_children: true
permalink: /ngx-laghu/troubleshooting/
---

# Troubleshoot ngx-laghu

Start with `nginx -t`, preserve the failing response headers and error-log interval, and select the runbook matching the failure domain.

- [Module startup and request handling](/ngx-laghu/troubleshooting/runtime/)
- [Workers, queues, and cache](/ngx-laghu/troubleshooting/workers-and-cache/)
- [RUM, Redis, and instrumentation](/ngx-laghu/troubleshooting/rum-and-redis/)
- [Transformations, CSP, and immutable routes](/ngx-laghu/troubleshooting/transformations/)

Do not delete cache or learning state before preserving diagnostics and confirming corruption.
