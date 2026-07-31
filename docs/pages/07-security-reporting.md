---
title: Security Reporting
nav_order: 7
permalink: /security-reporting/
---

# Security Reporting

Do not disclose a suspected vulnerability in a public issue.
Use [GitHub private vulnerability reporting](https://github.com/Code-Vedas/laghu/security/advisories/new); if that is unavailable, contact <admin@codevedas.com>.
Include the affected revision or release, deployment surface, reproduction steps, observed impact, and any proposed mitigation, but no production credentials or unrelated customer data.

## Security Model

Laghu is disabled by default in both native modules, bypasses authorization-bearing requests and private or `no-store` responses, and excludes API paths unless explicitly allowed. Transformations are bounded, validated, and fail open. Heavy codec and network work is isolated from web-server processes.

External font CSS fetching permits only administrator-configured exact HTTPS hosts and paths, validates DNS and redirects, and rejects private or reserved destinations. Redis credentials stay in administrator configuration and never enter browser output, policy keys, snapshots, or normal diagnostics. Browser instrumentation accepts bounded same-origin reports and stores aggregate opaque records rather than raw events or identifiers.

Review the product configuration before deployment: [`ngx-laghu`](/ngx-laghu/configure/), [`mod-laghu`](/mod-laghu/configure/), or [`laghu` server](/laghu-server/configure/).
